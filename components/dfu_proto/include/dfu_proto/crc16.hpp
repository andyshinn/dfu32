// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <span>

namespace dfu {

inline constexpr uint16_t kCrc16Init = 0xFFFF;

/// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xorout), the
/// variant used by Nordic SDK `crc16_compute()` and adafruit-nrfutil for the
/// init packet's firmware CRC. Pass the previous result as `crc` to continue
/// over data that arrives in pieces (e.g. reading an image from flash in blocks).
uint16_t crc16_ccitt(std::span<const uint8_t> data, uint16_t crc = kCrc16Init) noexcept;

}  // namespace dfu
