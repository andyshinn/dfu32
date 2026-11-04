// SPDX-License-Identifier: MIT
//
// Pre-START checks on an application image + init packet (SPEC.md §7.2). These
// run before the target is touched, because START erases the target's app.
#pragma once

#include <cstdint>

#include "dfu_proto/init_packet.hpp"

namespace dfu {

/// nRF52840 application-region ceilings with the Adafruit bootloader
/// (app starts after the SoftDevice, ends at 0xF4000 minus 40 KiB app data).
inline constexpr uint32_t kMaxAppSizeS140v6 = 802816;  // S140 6.1.1, app at 0x26000
inline constexpr uint32_t kMaxAppSizeS140v7 = 798720;  // S140 7.3.0, app at 0x27000
inline constexpr uint32_t kDefaultMaxAppSize = kMaxAppSizeS140v7;

enum class ImageError : uint8_t {
    Ok,
    Empty,
    NotWordAligned,  // bootloader requires sizes that are multiples of 4
    TooLarge,
    WrongDeviceType,  // .dat device_type != 0x0052
    NoSdReq,
    CrcMismatch,  // CRC16 of .bin != CRC in .dat
};

const char* to_string(ImageError e) noexcept;

/// Validates an app image described by its size and CRC16 against a parsed
/// init packet and the target's maximum application size.
ImageError check_app_image(const InitPacket& init, uint32_t bin_size, uint16_t bin_crc16,
                           uint32_t max_app_size = kDefaultMaxAppSize) noexcept;

}  // namespace dfu
