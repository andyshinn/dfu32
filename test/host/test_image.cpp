// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "dfu_proto/image.hpp"
#include "fixtures.hpp"

using dfu::check_app_image;
using dfu::ImageError;
using dfu::InitPacket;

namespace {
InitPacket rak4631_dat() {
    InitPacket p;
    REQUIRE(dfu::parse_init_packet(fixtures::read_fixture("meshcore-v1.18.0-rak_4631.dat"), p) ==
            dfu::InitPacketError::Ok);
    return p;
}
constexpr uint32_t kRak4631BinSize = 440924;  // MeshCore repeater v1.18.0 release
constexpr uint16_t kRak4631Crc = 18911;
}  // namespace

TEST_CASE("Real MeshCore image passes prechecks", "[image]") {
    const auto p = rak4631_dat();
    CHECK(check_app_image(p, kRak4631BinSize, kRak4631Crc) == ImageError::Ok);
    CHECK(check_app_image(p, kRak4631BinSize, kRak4631Crc, dfu::kMaxAppSizeS140v6) == ImageError::Ok);
}

TEST_CASE("Precheck failures", "[image]") {
    auto p = rak4631_dat();
    CHECK(check_app_image(p, 0, kRak4631Crc) == ImageError::Empty);
    CHECK(check_app_image(p, kRak4631BinSize + 2, kRak4631Crc) == ImageError::NotWordAligned);
    CHECK(check_app_image(p, dfu::kMaxAppSizeS140v7 + 4, kRak4631Crc) == ImageError::TooLarge);
    CHECK(check_app_image(p, dfu::kMaxAppSizeS140v7, kRak4631Crc) != ImageError::TooLarge);
    CHECK(check_app_image(p, dfu::kMaxAppSizeS140v7 + 4, kRak4631Crc, dfu::kMaxAppSizeS140v6) !=
          ImageError::TooLarge);
    CHECK(check_app_image(p, kRak4631BinSize, kRak4631Crc ^ 1) == ImageError::CrcMismatch);

    p.device_type = 0xFFFF;  // adafruit-nrfutil default when --dev-type is omitted
    CHECK(check_app_image(p, kRak4631BinSize, kRak4631Crc) == ImageError::WrongDeviceType);
}

TEST_CASE("ImageError strings are distinct", "[image]") {
    CHECK(std::string(dfu::to_string(ImageError::CrcMismatch)) == "DAT_CRC_MISMATCH");
    CHECK(std::string(dfu::to_string(ImageError::NotWordAligned)) == "IMAGE_NOT_WORD_ALIGNED");
}
