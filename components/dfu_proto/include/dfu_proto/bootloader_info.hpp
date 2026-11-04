// SPDX-License-Identifier: MIT
//
// Bootloader identification from the Device Information Service Firmware
// Revision string (SPEC.md §5.3). The Adafruit bootloader builds it as
// "<git describe> s140 <SoftDevice version>", e.g. "0.9.2-OTAFIX2.2-BP1.3 s140 6.1.1".
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dfu {

enum class BootloaderClass : uint8_t {
    Unknown,
    Otafix,          // BLE OTA recovery after failure/power cut; MTU 247
    UpstreamGe0_10,  // MTU 247; SYS_RESET returns to BLE OTA; power cut -> USB DFU
    UpstreamLe0_9,   // MTU 23 / 20-byte packets; any reset without app -> USB DFU
};

struct BootloaderInfo {
    BootloaderClass cls = BootloaderClass::Unknown;
    int major = -1, minor = -1, patch = -1;  // leading x.y.z of the git version
    std::string version;     // first token, e.g. "0.9.2-OTAFIX2.2-BP1.3"
    std::string sd_variant;  // e.g. "s140"
    std::string sd_version;  // e.g. "6.1.1"
    uint16_t sd_fwid = 0;    // 0 = unknown SoftDevice version
};

const char* to_string(BootloaderClass c) noexcept;

BootloaderInfo classify_bootloader(std::string_view firmware_revision);

/// SoftDevice firmware ID for an S140 version string, 0 if not in the table.
uint16_t s140_fwid(std::string_view sd_version) noexcept;

/// Largest Packet write the bootloader class accepts. Unknown is treated as
/// legacy (20 bytes): after a buttonless jump the link MTU comes from the app's
/// SoftDevice config and can exceed what an old bootloader's Packet char takes.
uint16_t max_packet_for(BootloaderClass c) noexcept;

}  // namespace dfu
