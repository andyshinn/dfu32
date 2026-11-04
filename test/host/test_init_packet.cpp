// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "dfu_proto/init_packet.hpp"
#include "fixtures.hpp"

using dfu::InitPacket;
using dfu::InitPacketError;
using dfu::parse_init_packet;

TEST_CASE("Parses MeshCore v1.18.0 RAK4631 repeater .dat", "[dat][fixture]") {
    const auto dat = fixtures::read_fixture("meshcore-v1.18.0-rak_4631.dat");
    REQUIRE(dat.size() == 14);
    InitPacket p;
    REQUIRE(parse_init_packet(dat, p) == InitPacketError::Ok);
    CHECK(p.device_type == 0x0052);
    CHECK(p.device_rev == 0xFFFF);
    CHECK(p.app_version == 0xFFFFFFFFu);
    REQUIRE(p.sd_req_count == 1);
    CHECK(p.sd_req[0] == 0x00B6);  // S140 6.1.1
    CHECK(p.firmware_crc16 == 18911);  // from the zip's manifest.json
    CHECK(p.accepts_softdevice(0x00B6));
    CHECK_FALSE(p.accepts_softdevice(0x0123));
}

TEST_CASE("Parses MeshCore v1.18.0 RAK3401 and XIAO .dat", "[dat][fixture]") {
    InitPacket rak3401;
    REQUIRE(parse_init_packet(fixtures::read_fixture("meshcore-v1.18.0-rak_3401.dat"), rak3401) ==
            InitPacketError::Ok);
    CHECK(rak3401.sd_req[0] == 0x00B6);
    CHECK(rak3401.firmware_crc16 == 42624);

    InitPacket xiao;
    REQUIRE(parse_init_packet(fixtures::read_fixture("meshcore-v1.18.0-xiao_nrf52.dat"), xiao) ==
            InitPacketError::Ok);
    CHECK(xiao.sd_req[0] == 0x0123);  // S140 7.3.0
    CHECK(xiao.firmware_crc16 == 57502);
    CHECK(xiao.accepts_softdevice(0x0123));
    CHECK_FALSE(xiao.accepts_softdevice(0x00B6));
}

TEST_CASE("Multiple sd_req entries and the 0xFFFE wildcard", "[dat]") {
    const std::vector<uint8_t> dat = {
        0x52, 0x00, 0xFF, 0xFF, 0x01, 0x02, 0x03, 0x04,  // type, rev, app_version
        0x03, 0x00,                                      // N = 3
        0xCA, 0x00, 0x00, 0x01, 0xFE, 0xFF,              // 0x00CA, 0x0100, 0xFFFE
        0x34, 0x12,                                      // crc
    };
    InitPacket p;
    REQUIRE(parse_init_packet(dat, p) == InitPacketError::Ok);
    CHECK(p.app_version == 0x04030201u);
    CHECK(p.sd_req_count == 3);
    CHECK(p.softdevice_requirements().size() == 3);
    CHECK(p.firmware_crc16 == 0x1234);
    CHECK(p.accepts_softdevice(0x00B6));  // via wildcard
}

TEST_CASE("Rejects malformed init packets", "[dat]") {
    InitPacket p;
    p.device_type = 0xABCD;  // must be untouched on failure

    SECTION("too short") {
        const std::vector<uint8_t> dat(13, 0);
        CHECK(parse_init_packet(dat, p) == InitPacketError::TooShort);
    }
    SECTION("N = 0") {
        const std::vector<uint8_t> dat = {0x52, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0, 0, 0, 0};
        CHECK(parse_init_packet(dat, p) == InitPacketError::NoSdReq);
    }
    SECTION("N larger than supported") {
        std::vector<uint8_t> dat = {0x52, 0, 0, 0, 0, 0, 0, 0, 17, 0x00};
        dat.resize(12 + 2 * 17, 0);
        CHECK(parse_init_packet(dat, p) == InitPacketError::TooManySdReq);
    }
    SECTION("N implies more bytes than present") {
        const std::vector<uint8_t> dat = {0x52, 0, 0, 0, 0, 0, 0, 0, 0x02, 0x00, 0xB6, 0x00, 0x11, 0x22};
        CHECK(parse_init_packet(dat, p) == InitPacketError::TooShort);
    }
    SECTION("extended (dfu-ver 0.6+) trailing data") {
        auto dat = fixtures::read_fixture("meshcore-v1.18.0-rak_4631.dat");
        dat.push_back(0x00);
        dat.push_back(0x00);
        CHECK(parse_init_packet(dat, p) == InitPacketError::UnsupportedLength);
    }
    CHECK(p.device_type == 0xABCD);
}

TEST_CASE("Parses MeshCore v1.18.0 RAKSHARE .dat (custom build)", "[dat][fixture]") {
    InitPacket p;
    REQUIRE(parse_init_packet(fixtures::read_fixture("meshcore-v1.18.0-rakshare.dat"), p) ==
            InitPacketError::Ok);
    CHECK(p.device_type == 0x0052);
    CHECK(p.sd_req_count == 1);
    CHECK(p.sd_req[0] == 0x00B6);
    CHECK(p.firmware_crc16 == 50803);
}
