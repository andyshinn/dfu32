// SPDX-License-Identifier: MIT
#include "dfu32_core/ota_image.hpp"

#include <cstring>

namespace dfu32::ota {
namespace {

std::string fixed_string(std::span<const uint8_t> field) {
    size_t n = 0;
    while (n < field.size() && field[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(field.data()), n);
}

uint32_t le32(const uint8_t* p) {
    return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24;
}

const char* chip_name(uint16_t id) {
    switch (id) {
        case 0x0000: return "ESP32";
        case 0x0002: return "ESP32-S2";
        case 0x0005: return "ESP32-C3";
        case 0x0009: return "ESP32-S3";
        case 0x000C: return "ESP32-C2";
        case 0x000D: return "ESP32-C6";
        case 0x0010: return "ESP32-H2";
        case 0x0012: return "ESP32-P4";
        case 0x0017: return "ESP32-C5";
        default: return nullptr;
    }
}

std::string app_file_hint(std::string_view board) {
    return "upload dfu32-" + std::string(board) + ".bin from a release, or dfu32.bin from a CI build";
}

std::string chip_text(uint16_t id) {
    const char* n = chip_name(id);
    return n != nullptr ? std::string(n) : "chip id " + std::to_string(id);
}

}  // namespace

std::string check_image_start(std::span<const uint8_t> head, const Expect& expect, ImageInfo& out) {
    out = ImageInfo{};
    if (head.size() < kInspectBytes) return "image too short to be DFU32 firmware";
    if (head[0] != kImageMagic) return "not an ESP-IDF app image (" + app_file_hint(expect.board) + ")";
    out.chip_id = static_cast<uint16_t>(head[12] | head[13] << 8);
    const uint8_t* desc = head.data() + kAppDescOffset;
    if (le32(desc) != kAppDescMagic) {
        return "app description missing, so not an ESP-IDF app image (" + app_file_hint(expect.board) + ")";
    }
    out.is_app = true;
    out.version = fixed_string(head.subspan(kAppDescOffset + 16, 32));
    out.project = fixed_string(head.subspan(kAppDescOffset + 48, 32));
    if (out.chip_id != expect.chip_id) {
        return "image is for " + chip_text(out.chip_id) + ", this device is " + chip_text(expect.chip_id);
    }
    if (out.project != expect.project) {
        return "image is the project \"" + out.project + "\", not \"" + std::string(expect.project) + "\"";
    }
    const uint8_t* tag = head.data() + kBoardTagOffset;
    if (std::memcmp(tag, kBoardTagMagic, sizeof(kBoardTagMagic)) == 0) {
        out.board = fixed_string(head.subspan(kBoardTagOffset + sizeof(kBoardTagMagic), sizeof(BoardTag::board)));
    }
    if (expect.allow_other_board) return {};
    if (out.board.empty()) {
        return "image has no DFU32 board tag (built before v0.4?); add ?force=1 to install it anyway";
    }
    if (out.board != expect.board) {
        return "image is for board \"" + out.board + "\", this device is \"" + std::string(expect.board) +
               "\" (same chip, different flash layout or pins); add ?force=1 to install it anyway";
    }
    return {};
}

void MergedProbe::feed(size_t offset, std::span<const uint8_t> chunk) {
    // Only contiguous data from 0 counts: a gap would leave the 0xFF run unchecked.
    if (offset != seen_) return;
    const auto byte_at = [&](size_t at, uint8_t& dst) {
        if (at >= offset && at < offset + chunk.size()) dst = chunk[at - offset];
    };
    byte_at(0x0, at_0_);
    byte_at(0x1000, at_1000_);
    byte_at(0x2000, at_2000_);
    byte_at(kPartitionTableOffset, pt_[0]);
    byte_at(kPartitionTableOffset + 1, pt_[1]);
    // Before the bootloader, a merged image is erased flash: track whether
    // everything below 0x2000 seen so far is 0xFF, up to the first 0xE9.
    for (size_t i = 0; i < chunk.size() && offset + i < 0x2000; ++i) {
        const size_t at = offset + i;
        if (at == 0x0 || at == 0x1000) continue;  // the candidate bootloader offsets
        if (chunk[i] != 0xFF) {
            if (at < 0x1000) ff_to_1000_ = false;
            ff_to_2000_ = false;
        }
    }
    seen_ = offset + chunk.size();
}

bool MergedProbe::merged() const {
    if (seen_ < kMergedProbeBytes) return false;
    if (pt_[0] != kPartitionMagic[0] || pt_[1] != kPartitionMagic[1]) return false;
    if (at_0_ == kImageMagic) return true;                                       // S3, C3, C6, ...
    if (at_0_ == 0xFF && at_1000_ == kImageMagic && ff_to_1000_) return true;    // ESP32, S2
    return at_0_ == 0xFF && at_1000_ == 0xFF && at_2000_ == kImageMagic && ff_to_2000_;  // C5
}

std::string merged_image_refusal(std::string_view board) {
    return "this is the merged image, for a fresh install over USB (it would also erase settings); " +
           app_file_hint(board);
}

}  // namespace dfu32::ota
