// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

#include "dfu_proto/protocol.hpp"

using dfu::Notification;
using dfu::Op;
using dfu::parse_notification;
using dfu::Status;

TEST_CASE("Control Point encoders", "[protocol]") {
    CHECK(dfu::encode_start() == std::array<uint8_t, 2>{0x01, 0x04});
    CHECK(dfu::encode_init_params(false) == std::array<uint8_t, 2>{0x02, 0x00});
    CHECK(dfu::encode_init_params(true) == std::array<uint8_t, 2>{0x02, 0x01});
    CHECK(dfu::encode_prn_request(8) == std::array<uint8_t, 3>{0x08, 0x08, 0x00});
    CHECK(dfu::encode_prn_request(0x1234) == std::array<uint8_t, 3>{0x08, 0x34, 0x12});
    CHECK(dfu::encode_op(Op::ReceiveFirmwareImage) == std::array<uint8_t, 1>{0x03});
    CHECK(dfu::encode_op(Op::SystemReset) == std::array<uint8_t, 1>{0x06});
}

TEST_CASE("START size triplet is SD, BL, APP little-endian", "[protocol]") {
    // 440924 = 0x0006BA5C
    const auto s = dfu::encode_start_sizes(440924);
    const std::array<uint8_t, 12> expected = {0, 0, 0, 0, 0, 0, 0, 0, 0x5C, 0xBA, 0x06, 0x00};
    CHECK(s == expected);
}

TEST_CASE("Parses response notifications", "[protocol]") {
    const std::vector<uint8_t> ok = {0x10, 0x01, 0x01};
    const auto n = parse_notification(ok);
    CHECK(n.kind == Notification::Kind::Response);
    CHECK(n.request == Op::StartDfu);
    CHECK(n.status == Status::Success);
    CHECK(dfu::is_success_for(n, Op::StartDfu));
    CHECK_FALSE(dfu::is_success_for(n, Op::InitDfuParams));

    const std::vector<uint8_t> crc = {0x10, 0x04, 0x05};
    const auto c = parse_notification(crc);
    CHECK(c.kind == Notification::Kind::Response);
    CHECK(c.status == Status::CrcError);
    CHECK_FALSE(dfu::is_success_for(c, Op::ValidateFirmware));
}

TEST_CASE("Unknown status is preserved and treated as failure", "[protocol]") {
    const std::vector<uint8_t> weird = {0x10, 0x03, 0x42};
    const auto n = parse_notification(weird);
    CHECK(n.kind == Notification::Kind::Response);
    CHECK(n.raw_status == 0x42);
    CHECK(n.status == Status::OperationFailed);
}

TEST_CASE("Parses REPORT_SIZE response and PRN", "[protocol]") {
    const std::vector<uint8_t> report = {0x10, 0x07, 0x01, 0x5C, 0xBA, 0x06, 0x00};
    const auto r = parse_notification(report);
    CHECK(r.kind == Notification::Kind::Response);
    CHECK(r.has_value);
    CHECK(r.value == 440924u);

    const std::vector<uint8_t> prn = {0x11, 0xA0, 0x07, 0x00, 0x00};
    const auto p = parse_notification(prn);
    CHECK(p.kind == Notification::Kind::PacketReceipt);
    CHECK(p.value == 1952u);  // 8 packets x 244 bytes
}

TEST_CASE("Malformed notifications are Invalid", "[protocol]") {
    const std::vector<std::vector<uint8_t>> bad = {
        {},
        {0x10},
        {0x10, 0x01},
        {0x10, 0x00, 0x01},              // request op 0 does not exist
        {0x10, 0x10, 0x01},              // response to a response
        {0x10, 0x07, 0x01, 0x00, 0x00},  // REPORT_SIZE success missing u32
        {0x11, 0x00, 0x00, 0x00},        // PRN missing a byte
        {0x42, 0x01, 0x01},
    };
    for (const auto& b : bad) {
        CHECK(parse_notification(b).kind == Notification::Kind::Invalid);
    }
}

TEST_CASE("String names", "[protocol]") {
    CHECK(std::string(dfu::to_string(Status::InvalidState)) == "INVALID_STATE");
    CHECK(std::string(dfu::to_string(Op::ActivateAndReset)) == "ACTIVATE_AND_RESET");
}
