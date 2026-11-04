// SPDX-License-Identifier: MIT
#include "dfu_proto/crc16.hpp"

namespace dfu {

// Same byte-wise formulation as Nordic SDK crc16_compute(), which is
// CRC-16/CCITT-FALSE (check value for "123456789" is 0x29B1).
uint16_t crc16_ccitt(std::span<const uint8_t> data, uint16_t crc) noexcept {
    for (const uint8_t b : data) {
        crc = static_cast<uint16_t>((crc >> 8) | (crc << 8));
        crc ^= b;
        crc ^= static_cast<uint16_t>((crc & 0xFFu) >> 4);
        crc ^= static_cast<uint16_t>(crc << 12);
        crc ^= static_cast<uint16_t>((crc & 0xFFu) << 5);
    }
    return crc;
}

}  // namespace dfu
