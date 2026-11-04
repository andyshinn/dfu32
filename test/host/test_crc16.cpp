// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include "dfu_proto/crc16.hpp"
#include "dfu_proto/init_packet.hpp"
#include "fixtures.hpp"

using dfu::crc16_ccitt;

namespace {
std::span<const uint8_t> bytes(const char* s) {
    return {reinterpret_cast<const uint8_t*>(s), std::char_traits<char>::length(s)};
}
}  // namespace

TEST_CASE("CRC-16/CCITT-FALSE standard check value", "[crc16]") {
    CHECK(crc16_ccitt(bytes("123456789")) == 0x29B1);
}

TEST_CASE("CRC16 of empty input is the init value", "[crc16]") {
    CHECK(crc16_ccitt({}) == 0xFFFF);
    CHECK(crc16_ccitt({}, 0x1234) == 0x1234);
}

TEST_CASE("CRC16 known single-byte vectors", "[crc16]") {
    const uint8_t zero = 0x00;
    const uint8_t ff = 0xFF;
    CHECK(crc16_ccitt({&zero, 1}) == 0xE1F0);
    CHECK(crc16_ccitt({&ff, 1}) == 0xFF00);
}

TEST_CASE("CRC16 is streamable across arbitrary splits", "[crc16]") {
    std::vector<uint8_t> data(4099);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i * 31 + 7);
    }
    const uint16_t whole = crc16_ccitt(data);
    for (size_t split : {size_t{0}, size_t{1}, size_t{244}, size_t{4096}, data.size()}) {
        const std::span<const uint8_t> all(data);
        const uint16_t part = crc16_ccitt(all.subspan(split), crc16_ccitt(all.first(split)));
        CHECK(part == whole);
    }
}

// Optional end-to-end check against a real release image. Point the env vars at
// an extracted adafruit-nrfutil DFU zip, e.g.:
//   DFU32_TEST_BIN=.../firmware.bin DFU32_TEST_DAT=.../firmware.dat ctest ...
TEST_CASE("CRC16 of a real image matches its .dat (optional)", "[crc16][fixture]") {
    const char* bin_path = std::getenv("DFU32_TEST_BIN");
    const char* dat_path = std::getenv("DFU32_TEST_DAT");
    if (bin_path == nullptr || dat_path == nullptr) {
        SKIP("DFU32_TEST_BIN / DFU32_TEST_DAT not set");
    }
    const auto bin = fixtures::read_file(bin_path);
    const auto dat = fixtures::read_file(dat_path);
    REQUIRE_FALSE(bin.empty());
    dfu::InitPacket pkt;
    REQUIRE(dfu::parse_init_packet(dat, pkt) == dfu::InitPacketError::Ok);
    CHECK(crc16_ccitt(bin) == pkt.firmware_crc16);
}
