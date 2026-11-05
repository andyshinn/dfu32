// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

#include "dfu_proto/init_packet.hpp"
#include "dfu_proto/staging_format.hpp"
#include "fixtures.hpp"

using dfu::StagingHeader;
using dfu::StagingHeaderError;

namespace {
std::string s(std::span<const uint8_t> d) { return std::string(d.begin(), d.end()); }
}  // namespace

TEST_CASE("CRC-32 matches zlib", "[staging]") {
    const std::string check = "123456789";
    CHECK(dfu::crc32_ieee({reinterpret_cast<const uint8_t*>(check.data()), check.size()}) == 0xCBF43926u);
    CHECK(dfu::crc32_ieee({}) == 0u);
}

TEST_CASE("Decodes the golden header written by tools/dfu32_stage_image.py", "[staging][fixture]") {
    const auto golden = fixtures::read_fixture("staging_header_golden.bin");
    REQUIRE(golden.size() == dfu::kStagingHeaderSize);
    StagingHeader h;
    REQUIRE(dfu::decode_staging_header(golden, h) == StagingHeaderError::Ok);
    CHECK(h.bin_size == 440924);
    CHECK(h.crc16 == 18911);
    CHECK(h.target_id == "attic-repeater");
    CHECK(h.board == "rak4631");
    CHECK(h.version == "v1.18.0-81dbae5");
    CHECK(h.staged_at == 1793750400ull);
    CHECK(h.manifest_url == "http://fw.lan/meshcore/rak4631-repeater/manifest.json");
    const auto dat = fixtures::read_fixture("meshcore-v1.18.0-rak_4631.dat");
    CHECK(s(h.dat_bytes()) == s(dat));

    dfu::InitPacket pkt;
    REQUIRE(dfu::parse_init_packet(h.dat_bytes(), pkt) == dfu::InitPacketError::Ok);
    CHECK(pkt.firmware_crc16 == h.crc16);
}

TEST_CASE("Encode/decode round trip reproduces the golden bytes", "[staging][fixture]") {
    const auto golden = fixtures::read_fixture("staging_header_golden.bin");
    StagingHeader h;
    REQUIRE(dfu::decode_staging_header(golden, h) == StagingHeaderError::Ok);
    std::array<uint8_t, dfu::kStagingHeaderSize> out{};
    REQUIRE(dfu::encode_staging_header(h, out) == StagingHeaderError::Ok);
    CHECK(std::vector<uint8_t>(out.begin(), out.end()) == golden);
}

TEST_CASE("Rejects damaged or empty headers", "[staging]") {
    StagingHeader h;
    std::vector<uint8_t> erased(4096, 0xFF);
    CHECK(dfu::decode_staging_header(erased, h) == StagingHeaderError::Erased);
    CHECK(dfu::decode_staging_header(std::vector<uint8_t>(100, 0), h) == StagingHeaderError::TooShort);

    auto golden = fixtures::read_fixture("staging_header_golden.bin");
    auto bad = golden;
    bad[0] = 'X';
    CHECK(dfu::decode_staging_header(bad, h) == StagingHeaderError::BadMagic);
    bad = golden;
    bad[0x100] ^= 1;  // inside version field
    CHECK(dfu::decode_staging_header(bad, h) == StagingHeaderError::BadCrc);
    bad = golden;
    bad[0x008] = 2;
    CHECK(dfu::decode_staging_header(bad, h) == StagingHeaderError::UnsupportedVersion);
}

TEST_CASE("Encode rejects oversize fields", "[staging]") {
    StagingHeader h;
    h.dat_size = 14;
    std::array<uint8_t, dfu::kStagingHeaderSize> out{};
    h.board = std::string(32, 'x');
    CHECK(dfu::encode_staging_header(h, out) == StagingHeaderError::FieldTooLong);
    h.board = "ok";
    h.dat_size = 0;
    CHECK(dfu::encode_staging_header(h, out) == StagingHeaderError::BadDatSize);
}
