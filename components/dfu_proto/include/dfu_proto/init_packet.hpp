// SPDX-License-Identifier: MIT
//
// Legacy DFU init packet (`.dat`), dfu-version 0.5 layout as produced by
// `adafruit-nrfutil dfu genpkg` and checked by the Adafruit bootloader
// (src/dfu_init.c). All fields little-endian:
//
//   0       u16  device_type      (Adafruit nRF52 bootloader: 0x0052; nRF54 port: 0x0054)
//   2       u16  device_rev       (only checked for SD/BL updates)
//   4       u32  app_version      (not checked)
//   8       u16  sd_req_count N   (must be >= 1)
//   10      u16  sd_req[N]        (must contain 0xFFFE or the installed SD FWID)
//   10+2N   u16  firmware CRC16   (CRC-16/CCITT-FALSE of the .bin, checked at VALIDATE)
//
// Extended (dfu-version 0.6+/signed) packets are longer and are not supported.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dfu {

inline constexpr uint16_t kAdafruitDeviceType = 0x0052;  // Adafruit nRF52 bootloader
inline constexpr uint16_t kNrf54DeviceType = 0x0054;     // meshtastic/nRF54_Bootloader
inline constexpr uint16_t kSdReqAny = 0xFFFE;
inline constexpr size_t kMaxSdReq = 16;
inline constexpr size_t kInitPacketMinSize = 14;                       // N = 1
inline constexpr size_t kInitPacketMaxSize = 12 + 2 * kMaxSdReq;       // 44

struct InitPacket {
    uint16_t device_type = 0;
    uint16_t device_rev = 0;
    uint32_t app_version = 0;
    uint8_t sd_req_count = 0;
    std::array<uint16_t, kMaxSdReq> sd_req{};
    uint16_t firmware_crc16 = 0;

    std::span<const uint16_t> softdevice_requirements() const noexcept {
        return {sd_req.data(), sd_req_count};
    }

    /// True if the bootloader would accept this packet on a device whose
    /// SoftDevice has firmware ID `installed_fwid` (0xFFFE in sd_req = any).
    bool accepts_softdevice(uint16_t installed_fwid) const noexcept;
};

enum class InitPacketError : uint8_t {
    Ok,
    TooShort,           // fewer than 14 bytes, or shorter than N implies
    UnsupportedLength,  // longer than 12 + 2N: extended/signed format
    NoSdReq,            // N == 0 (bootloader rejects)
    TooManySdReq,       // N > kMaxSdReq
};

const char* to_string(InitPacketError e) noexcept;

/// Structural parse. Does not apply bootloader policy; see check_app_image().
InitPacketError parse_init_packet(std::span<const uint8_t> data, InitPacket& out) noexcept;

}  // namespace dfu
