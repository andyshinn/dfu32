// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstring>
#include <vector>

#include "dfu32_core/ota_image.hpp"

using namespace dfu32::ota;

namespace {

// A synthetic image start laid out like ESP-IDF's: image header, segment
// header, esp_app_desc_t, then DFU32's board tag.
std::vector<uint8_t> image(uint16_t chip, const char* project, const char* version, const char* board) {
    std::vector<uint8_t> v(kInspectBytes + 64, 0);
    v[0] = kImageMagic;
    v[1] = 4;  // segments
    v[12] = static_cast<uint8_t>(chip & 0xFF);
    v[13] = static_cast<uint8_t>(chip >> 8);
    uint8_t* d = v.data() + kAppDescOffset;
    d[0] = 0x32;  // 0xABCD5432 little-endian
    d[1] = 0x54;
    d[2] = 0xCD;
    d[3] = 0xAB;
    std::strncpy(reinterpret_cast<char*>(d + 16), version, 32);
    std::strncpy(reinterpret_cast<char*>(d + 48), project, 32);
    if (board != nullptr) {
        std::memcpy(v.data() + kBoardTagOffset, kBoardTagMagic, sizeof(kBoardTagMagic));
        std::strncpy(reinterpret_cast<char*>(v.data() + kBoardTagOffset + 8), board, 32);
    }
    return v;
}

const Expect kS3{0x0009, "dfu32", "feather_esp32s3", false};

}  // namespace

TEST_CASE("OTA image: a matching DFU32 build is accepted", "[ota]") {
    ImageInfo info;
    CHECK(check_image_start(image(0x0009, "dfu32", "v0.4.0-3-gabc1234", "feather_esp32s3"), kS3, info).empty());
    CHECK(info.chip_id == 0x0009);
    CHECK(info.version == "v0.4.0-3-gabc1234");
    CHECK(info.project == "dfu32");
    CHECK(info.board == "feather_esp32s3");
}

TEST_CASE("OTA image: wrong chip, project or board is refused", "[ota]") {
    ImageInfo info;
    CHECK(check_image_start(image(0x0000, "dfu32", "v1", "feather_esp32_v2"), kS3, info) ==
          "image is for ESP32, this device is ESP32-S3");
    CHECK(check_image_start(image(0x0017, "dfu32", "v1", "xiao_esp32c5"), kS3, info) ==
          "image is for ESP32-C5, this device is ESP32-S3");
    CHECK(check_image_start(image(0x0009, "blink", "v1", nullptr), kS3, info) ==
          "image is the project \"blink\", not \"dfu32\"");
    const std::string other = check_image_start(image(0x0009, "dfu32", "v1", "esp32s3_n16r8"), kS3, info);
    CHECK(other.find("image is for board \"esp32s3_n16r8\"") == 0);
    CHECK(info.board == "esp32s3_n16r8");
    const std::string untagged = check_image_start(image(0x0009, "dfu32", "v1", nullptr), kS3, info);
    CHECK(untagged.find("no DFU32 board tag") != std::string::npos);
    CHECK(info.board.empty());

    // ?force=1 skips only the board check.
    Expect force = kS3;
    force.allow_other_board = true;
    CHECK(check_image_start(image(0x0009, "dfu32", "v1", "esp32s3_n16r8"), force, info).empty());
    CHECK(check_image_start(image(0x0009, "dfu32", "v1", nullptr), force, info).empty());
    CHECK_FALSE(check_image_start(image(0x0000, "dfu32", "v1", nullptr), force, info).empty());
}

TEST_CASE("OTA image: not an app image", "[ota]") {
    ImageInfo info;
    auto v = image(0x0009, "dfu32", "v1", "feather_esp32s3");
    CHECK(check_image_start(std::span<const uint8_t>(v).subspan(0, kInspectBytes - 1), kS3, info) ==
          "image too short to be DFU32 firmware");
    auto nomagic = v;
    nomagic[0] = 0xFF;
    CHECK(check_image_start(nomagic, kS3, info) ==
          "not an ESP-IDF app image (upload dfu32-feather_esp32s3.bin from a release, or dfu32.bin from a CI build)");
    CHECK_FALSE(info.is_app);
    auto nodesc = v;
    nodesc[kAppDescOffset] = 0;
    CHECK(check_image_start(nodesc, kS3, info).find("app description missing") == 0);
    CHECK(check_image_start(nodesc, kS3, info).find("dfu32-feather_esp32s3.bin") != std::string::npos);
    CHECK_FALSE(info.is_app);
    CHECK(check_image_start(v, kS3, info).empty());
    CHECK(info.is_app);
    // A wrong-chip app is still an app: the caller doesn't look for a merged image.
    CHECK_FALSE(check_image_start(image(0x0000, "dfu32", "v1", "feather_esp32_v2"), kS3, info).empty());
    CHECK(info.is_app);
    // Unterminated fields are cut at their size.
    auto longname = image(0x0009, "dfu32", "v1", "feather_esp32s3");
    std::memset(longname.data() + kAppDescOffset + 16, 'x', 32);
    CHECK(check_image_start(longname, kS3, info).empty());
    CHECK(info.version == std::string(32, 'x'));
}

namespace {

// The first kMergedProbeBytes of a merged image: 0xFF up to the bootloader at
// `boot`, a bootloader header there (magic, then a bootloader descriptor, not
// esp_app_desc_t), and the partition table magic at 0x8000.
std::vector<uint8_t> merged_start(size_t boot) {
    std::vector<uint8_t> v(kMergedProbeBytes + 100, 0xFF);
    v[boot] = kImageMagic;
    std::memset(v.data() + boot + 1, 0x00, 64);
    v[boot + kAppDescOffset] = 0x50;  // ESP_BOOTLOADER_DESC_MAGIC_BYTE
    v[kPartitionTableOffset] = 0xAA;
    v[kPartitionTableOffset + 1] = 0x50;
    return v;
}

bool probe_in_chunks(const std::vector<uint8_t>& v, size_t chunk) {
    MergedProbe p;
    for (size_t off = 0; off < v.size(); off += chunk) {
        p.feed(off, std::span<const uint8_t>(v).subspan(off, std::min(chunk, v.size() - off)));
    }
    return p.merged();
}

}  // namespace

TEST_CASE("OTA image: a merged image is recognised on every bootloader offset", "[ota]") {
    for (size_t boot : {size_t{0x0}, size_t{0x1000}, size_t{0x2000}}) {
        CAPTURE(boot);
        const auto v = merged_start(boot);
        ImageInfo info;
        CHECK_FALSE(check_image_start(v, kS3, info).empty());
        CHECK_FALSE(info.is_app);
        for (size_t chunk : {size_t{4096}, size_t{1000}, size_t{1}, v.size()}) {
            CAPTURE(chunk);
            CHECK(probe_in_chunks(v, chunk));
        }
    }
    CHECK(merged_image_refusal("xiao_esp32c5").find("merged image") != std::string::npos);
    CHECK(merged_image_refusal("xiao_esp32c5").find("dfu32-xiao_esp32c5.bin") != std::string::npos);
}

TEST_CASE("OTA image: other non-app uploads are not called merged", "[ota]") {
    // Too short to reach the partition table.
    auto v = merged_start(0x1000);
    v.resize(kMergedProbeBytes - 1);
    CHECK_FALSE(probe_in_chunks(v, 4096));
    // No partition table at 0x8000.
    v = merged_start(0x0);
    v[kPartitionTableOffset] = 0;
    CHECK_FALSE(probe_in_chunks(v, 4096));
    // Something other than erased flash before the bootloader.
    v = merged_start(0x1000);
    v[0x800] = 0x12;
    CHECK_FALSE(probe_in_chunks(v, 4096));
    v = merged_start(0x2000);
    v[0x1800] = 0x12;
    CHECK_FALSE(probe_in_chunks(v, 4096));
    // No bootloader at any of the offsets.
    v = std::vector<uint8_t>(kMergedProbeBytes, 0xFF);
    v[kPartitionTableOffset] = 0xAA;
    v[kPartitionTableOffset + 1] = 0x50;
    CHECK_FALSE(probe_in_chunks(v, 4096));
    // A zip (a MeshCore DFU package uploaded here by mistake).
    v = std::vector<uint8_t>(kMergedProbeBytes, 0x00);
    v[0] = 'P';
    v[1] = 'K';
    CHECK_FALSE(probe_in_chunks(v, 4096));
    // Fed out of order: ignored rather than misread.
    v = merged_start(0x0);
    MergedProbe p;
    p.feed(4096, std::span<const uint8_t>(v).subspan(4096));
    CHECK_FALSE(p.merged());
}
