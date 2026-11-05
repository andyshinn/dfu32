// SPDX-License-Identifier: MIT
//
// NimBLE (esp-nimble-cpp) side of DFU32: scanning, entering the bootloader via
// Bluefruit buttonless DFU, identifying the bootloader, and running a transfer
// through dfu::run_transfer(). See SPEC.md §5–§6.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dfu_proto/bootloader_info.hpp"
#include "dfu_proto/init_packet.hpp"
#include "dfu_proto/retry.hpp"
#include "dfu_proto/transfer.hpp"

namespace dfu32::ble {

/// A BLE address. `val` is LSB-first (NimBLE order); text form is MSB-first
/// ("C2:4E:11:9A:03:7F"), matching MeshCore's `start ota` reply.
struct Mac {
    std::array<uint8_t, 6> val{};
    uint8_t type = 1;  // BLE_ADDR_RANDOM; nRF52 default is random static

    static bool parse(const std::string& text, uint8_t type, Mac& out);
    std::string str() const;
    /// Bootloader address after a 0xA8 reset: LSB + 1, uint8 wrap, no carry.
    Mac plus_one() const;
    bool same_bytes(const uint8_t* lsb_first) const;
};

struct Advert {
    Mac mac;
    std::string name;
    int rssi = 0;
    bool dfu_service = false;  // advertises the legacy DFU service UUID
    bool connectable = false;
};

/// Brings up NimBLE as a central. Safe to call once at boot.
bool init();

/// Scans for `duration_ms` and returns every advertiser seen, strongest first.
std::vector<Advert> scan(uint32_t duration_ms);

struct LinkInfo {
    Mac address;                     // address the bootloader was reached at
    bool via_buttonless = false;     // target was in the app and we triggered the jump
    bool found_at_plus_one = false;  // bootloader at MAC+1 (0xA8 reset path)
    uint16_t version_char = 0;       // 0x0008 expected in the bootloader
    uint16_t mtu = 0;
    uint8_t tx_phy = 0, rx_phy = 0;  // 1 = 1M, 2 = 2M (0 = unknown)
    std::string dis_fw_rev;
    std::string dis_model;
    dfu::BootloaderInfo bootloader;
    uint16_t packet_size = 0;        // what DFU32 will use
    uint32_t find_ms = 0, jump_ms = 0, connect_ms = 0;
    uint16_t conn_interval = 0;      // 1.25 ms units, as negotiated with the bootloader
    uint16_t conn_latency = 0;
    uint16_t conn_timeout = 0;       // 10 ms units
};

/// Connection interval request. 0 = default 20 ms; otherwise request
/// exactly this many milliseconds (rounded down to 1.25 ms steps, min 7.5 ms).
struct ConnParams {
    uint16_t interval_ms = 0;
};

struct ProbeReport {
    bool ok = false;
    std::string error;
    LinkInfo link;
    bool reset_sent = false;
};

/// Non-destructive link test (the "BLE frame size" tool): finds the target at
/// `app` (or MAC+1), enters the bootloader via buttonless DFU if needed,
/// negotiates MTU/PHY/DLE, reads Version + DIS, computes the packet size, then
/// (if `reset_after`) sends SYS_RESET so the target reboots into its app.
/// Never sends START, so nothing is erased.
ProbeReport probe(const Mac& app, uint32_t scan_ms, bool reset_after = true, ConnParams conn = {});

struct FlashOptions {
    uint32_t scan_ms = 600000;   // how long to wait for the target on attempt 1 (arm timeout)
    uint16_t prn = 8;            // capped at dfu::kMaxSafePrn unless force_prn
    bool force_prn = false;
    uint32_t start_settle_ms = 500;
    uint16_t max_packet = 0;     // 0 = automatic; else cap (e.g. 20 for testing)
    ConnParams conn{};           // interval for attempt 1; retries may lengthen it
    uint8_t attempts = 3;        // total attempts (SPEC.md §6.5)
};

struct AttemptSummary {
    uint8_t number = 0;
    dfu::AttemptParams params;   // as used (PRN after the cap)
    dfu::FailureKind failure = dfu::FailureKind::None;
    std::string error;           // empty on success
    dfu::TransferResult transfer;
    bool reached_bootloader = false;
};

struct FlashReport {
    bool ok = false;
    std::string error;                 // from the last attempt
    dfu::FailureKind failure = dfu::FailureKind::None;
    LinkInfo link;                     // last attempt that reached the bootloader
    dfu::TransferResult transfer;      // last attempt's transfer
    std::vector<AttemptSummary> attempts;
    bool target_erased = false;        // any attempt got as far as START
    bool sys_reset_after_failure = false;
    uint32_t total_ms = 0;
};

/// Flashes the image with automatic retries (SPEC.md §6.5). Each attempt checks
/// the device type and SoftDevice requirement against the bootloader before
/// START; mismatches stop immediately and SYS_RESET returns an untouched target
/// to its app. After any failure with the bootloader still connected, SYS_RESET
/// clears the session so the next attempt can START. A stream overrun or link
/// loss makes the next attempt more conservative (dfu::degrade).
FlashReport flash(const Mac& app, dfu::IImageSource& image, uint32_t image_size,
                  std::span<const uint8_t> init_packet, const dfu::InitPacket& init,
                  const FlashOptions& opts);

/// Connects to the target's bootloader (MAC or MAC+1) and sends SYS_RESET.
/// For recovering a bootloader left mid-session by an aborted run.
bool reset_bootloader(const Mac& app, uint32_t scan_ms, std::string& error);

}  // namespace dfu32::ble
