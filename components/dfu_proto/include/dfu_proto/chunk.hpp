// SPDX-License-Identifier: MIT
//
// Packet sizing for the DFU Packet characteristic (SPEC.md §6.3).
#pragma once

#include <cstdint>

namespace dfu {

inline constexpr uint16_t kAttMtuMin = 23;
inline constexpr uint16_t kAttHeaderSize = 3;
/// Packet characteristic max length: Adafruit bootloader <= 0.9.2 (MTU 23).
inline constexpr uint16_t kPacketMaxLegacy = 20;
/// Packet characteristic max length: OTAFIX 2.x and upstream >= 0.10 (MTU 247).
inline constexpr uint16_t kPacketMax = 244;

/// Largest data-packet size usable at `att_mtu` given the characteristic's max
/// length: min(max_char_len, mtu - 3), rounded down to a multiple of 4 (the
/// bootloader rejects other sizes). Returns 0 if `att_mtu` < 23 or the result
/// would be smaller than 4.
constexpr uint16_t packet_size_for_mtu(uint16_t att_mtu, uint16_t max_char_len = kPacketMax) noexcept {
    if (att_mtu < kAttMtuMin) {
        return 0;
    }
    uint16_t n = static_cast<uint16_t>(att_mtu - kAttHeaderSize);
    if (max_char_len < n) {
        n = max_char_len;
    }
    n = static_cast<uint16_t>(n & ~uint16_t{3});
    return n >= 4 ? n : 0;
}

struct ChunkPlan {
    uint32_t image_size = 0;
    uint16_t packet_size = 0;
    uint32_t packet_count = 0;   // including the last (possibly shorter) packet
    uint16_t last_packet_size = 0;
};

/// Splits an image into Packet writes. Fails (returns false) if the image is
/// empty or not a multiple of 4, or if `packet_size` is 0 or not a multiple of 4.
/// Every packet, including the last, is then a multiple of 4.
constexpr bool plan_chunks(uint32_t image_size, uint16_t packet_size, ChunkPlan& out) noexcept {
    if (image_size == 0 || (image_size & 3u) != 0 || packet_size == 0 || (packet_size & 3u) != 0) {
        return false;
    }
    out.image_size = image_size;
    out.packet_size = packet_size;
    out.packet_count = (image_size + packet_size - 1) / packet_size;
    const uint32_t rem = image_size % packet_size;
    out.last_packet_size = static_cast<uint16_t>(rem == 0 ? packet_size : rem);
    return true;
}

}  // namespace dfu
