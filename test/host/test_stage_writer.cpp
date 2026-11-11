// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "dfu32_core/stage_writer.hpp"
#include "dfu_proto/crc16.hpp"
#include "dfu_proto/image.hpp"
#include "dfu_proto/staging_format.hpp"
#include "sha256_ref.hpp"

using dfu32::Err;

namespace {

// NOR-flash model: erase sets 0xFF, writes can only clear bits.
struct FakeRegion final : dfu32::IFlashRegion {
    explicit FakeRegion(uint32_t size) : mem(size, 0x00) {}  // starts dirty: must be erased before writing
    uint32_t size() const override { return static_cast<uint32_t>(mem.size()); }
    bool erase(uint32_t off, uint32_t len) override {
        ++erases;
        if (fail_erase_after && erases > fail_erase_after) return false;
        if (fail_erase || off % dfu32::kFlashSector || len % dfu32::kFlashSector || off + len > mem.size()) return false;
        std::fill_n(mem.begin() + off, len, 0xFF);
        return true;
    }
    bool write(uint32_t off, std::span<const uint8_t> d) override {
        if (fail_write_at && off >= fail_write_at) return false;
        if (off + d.size() > mem.size()) return false;
        for (size_t i = 0; i < d.size(); ++i) {
            if ((mem[off + i] & d[i]) != d[i]) unerased_write = true;
            mem[off + i] &= d[i];
        }
        return true;
    }
    bool read(uint32_t off, std::span<uint8_t> out) override {
        if (off + out.size() > mem.size()) return false;
        std::copy_n(mem.begin() + off, out.size(), out.begin());
        if (corrupt_readback && off >= dfu::kStagingDataOffset && !out.empty()) out[0] ^= 0x01;
        return true;
    }
    std::vector<uint8_t> mem;
    int erases = 0;
    bool fail_erase = false;
    int fail_erase_after = 0;
    uint32_t fail_write_at = 0;
    bool corrupt_readback = false;
    bool unerased_write = false;
};

std::vector<uint8_t> make_bin(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 31 + (i >> 9));
    return v;
}

std::vector<uint8_t> make_dat(const std::vector<uint8_t>& bin, uint16_t device_type = 0x0052) {
    const uint16_t crc = dfu::crc16_ccitt(bin);
    return {static_cast<uint8_t>(device_type), static_cast<uint8_t>(device_type >> 8), 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0x01, 0x00, 0xB6, 0x00, static_cast<uint8_t>(crc), static_cast<uint8_t>(crc >> 8)};
}

struct Fixture {
    std::vector<uint8_t> bin = make_bin(244 * 100 + 4);
    std::vector<uint8_t> dat = make_dat(bin);
    dfu32::Manifest m;
    dfu32::StageMeta meta;
    FakeRegion region{0xD0000};
    RefSha256 sha;

    Fixture() {
        m.schema = 1;
        m.version = "v1.18.0-81dbae5";
        m.boards = {"rak4631"};
        m.bin = {"http://fw.lan/firmware.bin", static_cast<uint32_t>(bin.size()), RefSha256::of(bin)};
        m.dat = {"http://fw.lan/firmware.dat", static_cast<uint32_t>(dat.size()), RefSha256::of(dat)};
        meta.target_id = "attic-repeater";
        meta.board = "RAK4631";
        meta.manifest_url = "http://fw.lan/manifest.json";
        meta.manifest_sha256 = RefSha256::of("manifest");
        meta.staged_at = 1762300000;
    }

    dfu32::Status stage(size_t chunk = 1000) {
        dfu32::StagingWriter w(region, sha);
        if (auto s = w.begin(m, dat, meta); !s.ok()) return s;
        for (size_t off = 0; off < bin.size(); off += chunk) {
            const size_t n = std::min(chunk, bin.size() - off);
            if (auto s = w.write({bin.data() + off, n}); !s.ok()) return s;
        }
        return w.finish();
    }
};

}  // namespace

TEST_CASE("Stage writer: happy path writes data then a valid header", "[stage]") {
    Fixture f;
    const auto s = f.stage(1460);  // TCP-ish chunk size, unaligned to sectors
    INFO(s.str());
    REQUIRE(s.ok());
    CHECK_FALSE(f.region.unerased_write);
    CHECK(std::equal(f.bin.begin(), f.bin.end(), f.region.mem.begin() + dfu::kStagingDataOffset));
    dfu::StagingHeader h;
    REQUIRE(dfu::decode_staging_header({f.region.mem.data(), dfu::kStagingHeaderSize}, h) ==
            dfu::StagingHeaderError::Ok);
    CHECK(h.bin_size == f.bin.size());
    CHECK(h.bin_sha256 == f.m.bin.sha256);
    CHECK(std::vector<uint8_t>(h.dat_bytes().begin(), h.dat_bytes().end()) == f.dat);
    CHECK(h.crc16 == dfu::crc16_ccitt(f.bin));
    CHECK(h.target_id == "attic-repeater");
    CHECK(h.board == "RAK4631");
    CHECK(h.version == "v1.18.0-81dbae5");
    CHECK(h.manifest_url == "http://fw.lan/manifest.json");
    CHECK(h.staged_at == 1762300000);
    CHECK(dfu32::same_image(f.m, h));
}

TEST_CASE("Stage writer: precheck failures leave the old image alone", "[stage]") {
    Fixture f;
    REQUIRE(f.stage().ok());
    const auto before = f.region.mem;
    const int erases = f.region.erases;

    SECTION("board") {
        f.meta.board = "heltec_t114";
        CHECK(f.stage().code == Err::BoardMismatch);
    }
    SECTION("no board on the target: not compared, and a one-board manifest names the image") {
        f.meta.board.clear();
        REQUIRE(f.stage().ok());
        dfu::StagingHeader h;
        REQUIRE(dfu::decode_staging_header({f.region.mem.data(), dfu::kStagingHeaderSize}, h) ==
                dfu::StagingHeaderError::Ok);
        CHECK(h.board == "rak4631");  // so an ID given to the target later is still checked at flash
        f.m.boards = {"rak4631", "rak3401"};
        CHECK(dfu32::staged_board(f.m, f.meta).empty());  // several boards: can't say which
        f.meta.board = "RAK3401";
        CHECK(dfu32::staged_board(f.m, f.meta) == "RAK3401");
        return;  // it staged, so the "old image untouched" checks below don't apply
    }
    SECTION(".dat hash") {
        f.m.dat.sha256[0] ^= 1;
        CHECK(f.stage().code == Err::HashMismatch);
    }
    SECTION(".dat size") {
        f.m.dat.size = 16;
        CHECK(f.stage().code == Err::SizeMismatch);
    }
    SECTION(".dat layout") {
        f.dat.push_back(0);  // 15 bytes: neither N=1 nor N=2
        f.m.dat = {f.m.dat.url, 15, RefSha256::of(f.dat)};
        CHECK(f.stage().code == Err::DatInvalid);
    }
    SECTION("unknown device type") {
        f.dat = make_dat(f.bin, 0xFFFF);
        f.m.dat.sha256 = RefSha256::of(f.dat);
        CHECK(f.stage().code == Err::DatInvalid);
    }
    SECTION("manifest init disagrees") {
        f.m.init.present = true;
        f.m.init.has_crc16 = true;
        f.m.init.crc16 = 0x1234;
        CHECK(f.stage().code == Err::DatInvalid);
    }
    SECTION("alignment") {
        f.m.bin.size = 1022;
        CHECK(f.stage().code == Err::ImageNotWordAligned);
    }
    SECTION("target size limit") {
        f.meta.max_app_size = 4096;
        CHECK(f.stage().code == Err::ImageTooLarge);
    }
    SECTION("default nRF52 limit") {
        f.m.bin.size = dfu::kDefaultMaxAppSize + 4;
        CHECK(f.stage().code == Err::ImageTooLarge);
    }
    SECTION("staging capacity") {
        f.region = FakeRegion(0x6000);  // header sector + 20 KB: the 24 KB image doesn't fit
        f.region.erases = erases;
        CHECK(f.stage().code == Err::ImageTooLarge);
    }
    SECTION("header field too long") {
        f.meta.target_id = std::string(32, 't');
        CHECK(f.stage().code == Err::ManifestInvalid);
    }
    if (f.region.mem.size() == before.size()) CHECK(f.region.mem == before);
    CHECK(f.region.erases == erases);
}

TEST_CASE("Stage writer: nRF54 images skip the nRF52 size ceiling", "[stage]") {
    Fixture f;
    f.bin = make_bin(dfu::kDefaultMaxAppSize + 4096);
    f.dat = make_dat(f.bin, 0x0054);
    f.m.bin = {f.m.bin.url, static_cast<uint32_t>(f.bin.size()), RefSha256::of(f.bin)};
    f.m.dat = {f.m.dat.url, static_cast<uint32_t>(f.dat.size()), RefSha256::of(f.dat)};
    f.region = FakeRegion(0x100000);
    CHECK(f.stage(4096).ok());
}

TEST_CASE("Stage writer: download problems", "[stage]") {
    Fixture f;
    SECTION("body longer than the manifest says") {
        f.m.bin.size -= 4;
        CHECK(f.stage().code == Err::SizeMismatch);
    }
    SECTION("body shorter than the manifest says") {
        f.bin.resize(f.bin.size() - 4);
        CHECK(f.stage().code == Err::SizeMismatch);
    }
    SECTION("wrong bytes") {
        f.bin[100] ^= 0xFF;
        CHECK(f.stage().code == Err::HashMismatch);
    }
    SECTION("flash write failure") {
        f.region.fail_write_at = dfu::kStagingDataOffset + 8192;
        CHECK(f.stage().code == Err::StagingIo);
    }
    SECTION("erase failure") {
        f.region.fail_erase = true;
        CHECK(f.stage().code == Err::StagingIo);
    }
    SECTION("erase failure mid-body") {
        f.region.fail_erase_after = 3;  // header + two data sectors, then fail
        CHECK(f.stage().code == Err::StagingIo);
    }
    SECTION("read-back differs") {
        f.region.corrupt_readback = true;
        CHECK(f.stage().code == Err::StagingIo);
    }
    SECTION("hash engine failure") {
        dfu32::StagingWriter w(f.region, f.sha);
        REQUIRE(w.begin(f.m, f.dat, f.meta).ok());
        f.sha.fail_updates = true;
        CHECK(w.write({f.bin.data(), 64}).code == Err::StagingIo);
        CHECK(w.write({f.bin.data(), 64}).code == Err::StagingIo);  // stays failed
    }
    // Whatever failed, the header was erased, so nothing reads as staged.
    dfu::StagingHeader h;
    CHECK(dfu::decode_staging_header({f.region.mem.data(), dfu::kStagingHeaderSize}, h) !=
          dfu::StagingHeaderError::Ok);
}

TEST_CASE("Stage writer: reports when the previous image was invalidated", "[stage]") {
    Fixture f;
    dfu32::StagingWriter w(f.region, f.sha);
    f.meta.board = "nope";
    CHECK_FALSE(w.begin(f.m, f.dat, f.meta).ok());
    CHECK_FALSE(w.invalidated_previous());  // precheck failed: nothing touched
    f.meta.board = "rak4631";
    REQUIRE(w.begin(f.m, f.dat, f.meta).ok());
    CHECK(w.invalidated_previous());  // header erased even though no byte was written yet
    CHECK(w.written() == 0);
}

TEST_CASE("Stage writer: reusing a staged image refreshes its header", "[stage]") {
    Fixture f;
    REQUIRE(f.stage().ok());
    dfu::StagingHeader h;
    REQUIRE(dfu::decode_staging_header({f.region.mem.data(), dfu::kStagingHeaderSize}, h) ==
            dfu::StagingHeaderError::Ok);
    const std::vector<uint8_t> data(f.region.mem.begin() + dfu::kStagingDataOffset, f.region.mem.end());

    bool rewritten = true;
    REQUIRE(dfu32::refresh_staged_header(f.region, h, f.m, f.meta, rewritten).ok());
    CHECK_FALSE(rewritten);  // same owner: no flash write

    f.meta.target_id = "garage-repeater";
    f.meta.manifest_url = "http://fw.lan/garage/manifest.json";
    f.meta.staged_at = 1762400000;
    REQUIRE(dfu32::refresh_staged_header(f.region, h, f.m, f.meta, rewritten).ok());
    CHECK(rewritten);
    CHECK_FALSE(f.region.unerased_write);
    dfu::StagingHeader h2;
    REQUIRE(dfu::decode_staging_header({f.region.mem.data(), dfu::kStagingHeaderSize}, h2) ==
            dfu::StagingHeaderError::Ok);
    CHECK(h2.target_id == "garage-repeater");
    CHECK(h2.manifest_url == "http://fw.lan/garage/manifest.json");
    CHECK(h2.staged_at == 1762400000);
    CHECK(dfu32::same_image(f.m, h2));
    CHECK(std::equal(data.begin(), data.end(), f.region.mem.begin() + dfu::kStagingDataOffset));

    f.meta.target_id = std::string(40, 'x');
    CHECK(dfu32::refresh_staged_header(f.region, h2, f.m, f.meta, rewritten).code == Err::ManifestInvalid);
    CHECK_FALSE(rewritten);  // refused before erasing
}

TEST_CASE("Stage writer: CRC16 mismatch between .bin and .dat", "[stage]") {
    Fixture f;
    f.dat = make_dat(make_bin(16));  // valid layout, CRC of some other image
    f.m.dat.sha256 = RefSha256::of(f.dat);
    CHECK(f.stage().code == Err::DatCrcMismatch);
}

TEST_CASE("Stage writer: misuse is reported, not crashed on", "[stage]") {
    Fixture f;
    dfu32::StagingWriter w(f.region, f.sha);
    CHECK(w.write({f.bin.data(), 4}).code == Err::StagingIo);
    CHECK(w.finish().code == Err::StagingIo);
}

TEST_CASE("same_image compares hashes and sizes", "[stage]") {
    Fixture f;
    REQUIRE(f.stage().ok());
    dfu::StagingHeader h;
    REQUIRE(dfu::decode_staging_header({f.region.mem.data(), dfu::kStagingHeaderSize}, h) ==
            dfu::StagingHeaderError::Ok);
    auto other = f.m;
    other.bin.sha256[5] ^= 1;
    CHECK_FALSE(dfu32::same_image(other, h));
    other = f.m;
    other.dat.sha256[0] ^= 1;
    CHECK_FALSE(dfu32::same_image(other, h));
}

// ---- uploads (POST /api/v1/staging): no manifest ----------------------------------

namespace {

struct Upload : Fixture {
    dfu32::UploadInfo up;
    Upload() {
        up.bin_size = static_cast<uint32_t>(bin.size());
        up.version = "v1.18.0-81dbae5";
        meta = {};
        meta.manifest_url = "upload:RAK_4631_repeater-v1.18.0-81dbae5.zip";
        meta.staged_at = 1762300000;
    }
    dfu32::Status upload(size_t chunk = 1460) {
        dfu32::StagingWriter w(region, sha);
        if (auto s = w.begin_upload(dat, up, meta); !s.ok()) return s;
        for (size_t off = 0; off < bin.size(); off += chunk) {
            const size_t n = std::min(chunk, bin.size() - off);
            if (auto s = w.write({bin.data() + off, n}); !s.ok()) return s;
        }
        return w.finish();
    }
    dfu::StagingHeader header() {
        dfu::StagingHeader h;
        REQUIRE(dfu::decode_staging_header({region.mem.data(), dfu::kStagingHeaderSize}, h) ==
                dfu::StagingHeaderError::Ok);
        return h;
    }
};

}  // namespace

TEST_CASE("Stage writer: an upload records the SHA-256 of what arrived", "[stage][upload]") {
    Upload u;
    const auto s = u.upload();
    INFO(s.str());
    REQUIRE(s.ok());
    CHECK_FALSE(u.region.unerased_write);
    CHECK(std::equal(u.bin.begin(), u.bin.end(), u.region.mem.begin() + dfu::kStagingDataOffset));
    const auto h = u.header();
    CHECK(h.bin_size == u.bin.size());
    CHECK(h.bin_sha256 == RefSha256::of(u.bin));
    CHECK(h.dat_sha256 == RefSha256::of(u.dat));
    CHECK(std::vector<uint8_t>(h.dat_bytes().begin(), h.dat_bytes().end()) == u.dat);
    CHECK(h.crc16 == dfu::crc16_ccitt(u.bin));
    CHECK(h.version == "v1.18.0-81dbae5");
    CHECK(h.target_id.empty());  // not tied to a target
    CHECK(h.board.empty());      // no board: DFU32 doesn't compare it with the target's
    CHECK(h.manifest_url == "upload:RAK_4631_repeater-v1.18.0-81dbae5.zip");
    CHECK(h.manifest_sha256 == dfu32::Sha256{});
    CHECK(h.staged_at == 1762300000);
}

TEST_CASE("Stage writer: an upload may name a board and the .bin's SHA-256", "[stage][upload]") {
    Upload u;
    u.meta.board = "rak4631";
    u.up.has_sha256 = true;
    u.up.bin_sha256 = RefSha256::of(u.bin);
    REQUIRE(u.upload().ok());
    CHECK(u.header().board == "rak4631");

    Upload bad;
    bad.up.has_sha256 = true;
    bad.up.bin_sha256 = RefSha256::of(bad.bin);
    bad.up.bin_sha256[0] ^= 1;
    CHECK(bad.upload().code == Err::HashMismatch);
    dfu::StagingHeader h;
    CHECK(dfu::decode_staging_header({bad.region.mem.data(), dfu::kStagingHeaderSize}, h) !=
          dfu::StagingHeaderError::Ok);  // nothing vouches for it
}

TEST_CASE("Stage writer: the .dat's CRC16 vouches for an uploaded .bin", "[stage][upload]") {
    Upload u;
    u.dat = make_dat(make_bin(16));  // valid layout, another image's CRC
    CHECK(u.upload().code == Err::DatCrcMismatch);
    dfu::StagingHeader h;
    CHECK(dfu::decode_staging_header({u.region.mem.data(), dfu::kStagingHeaderSize}, h) !=
          dfu::StagingHeaderError::Ok);
}

TEST_CASE("Stage writer: upload prechecks leave the old image alone", "[stage][upload]") {
    const auto refused = [](auto tweak, Err want) {
        Upload u;
        tweak(u);
        dfu32::StagingWriter w(u.region, u.sha);
        const auto s = w.begin_upload(u.dat, u.up, u.meta);
        INFO(s.str());
        CHECK(s.code == want);
        CHECK(u.region.erases == 0);
        CHECK_FALSE(w.invalidated_previous());
    };
    refused([](Upload& u) { u.up.version.clear(); }, Err::ManifestInvalid);
    refused([](Upload& u) { u.up.version = std::string(64, 'v'); }, Err::ManifestInvalid);
    refused([](Upload& u) { u.meta.board = std::string(32, 'b'); }, Err::ManifestInvalid);
    refused([](Upload& u) { u.dat.clear(); }, Err::DatInvalid);
    refused([](Upload& u) { u.dat.resize(65, 0); }, Err::DatTooLong);
    refused([](Upload& u) { u.dat.pop_back(); }, Err::DatInvalid);
    refused([](Upload& u) { u.dat = make_dat(u.bin, 0x0099); }, Err::DatInvalid);
    refused([](Upload& u) { u.up.bin_size = 0; }, Err::SizeMismatch);
    refused([](Upload& u) { u.up.bin_size = 1002; }, Err::ImageNotWordAligned);
    refused([](Upload& u) { u.up.bin_size = dfu::default_max_app_size(0x0052) + 4; }, Err::ImageTooLarge);
    refused([](Upload& u) { u.meta.max_app_size = 4096; }, Err::ImageTooLarge);
    refused([](Upload& u) { u.dat = make_dat(u.bin, 0x0054); u.up.bin_size = 0xD0000; }, Err::ImageTooLarge);  // staging capacity
}

TEST_CASE("Stage writer: an upload must be exactly its announced size", "[stage][upload]") {
    Upload shorter;
    shorter.up.bin_size += 4;
    CHECK(shorter.upload().code == Err::SizeMismatch);  // finish(): fewer bytes arrived

    Upload longer;
    longer.up.bin_size -= 4;
    CHECK(longer.upload().code == Err::SizeMismatch);  // write(): runs past it
}
