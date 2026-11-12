// SPDX-License-Identifier: MIT
//
// NimBLE (esp-nimble-cpp) side of DFU32: scanning, entering the bootloader via
// Bluefruit buttonless DFU, identifying the bootloader, and running a transfer
// through dfu::run_transfer(). See SPEC.md §5–§6.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
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
/// Returns early, with what it has seen so far, on request_abort().
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

/// Connection interval request. 0 = default (dfu::kDefaultIntervalMs, 30 ms); otherwise request
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

/// Live progress for the orchestrator (GET /api/v1/status).
struct FlashProgress {
    uint8_t attempt = 0;
    const char* phase = "";      // SCAN, CONNECT, START, STREAM, RETRY
    uint32_t bytes_acked = 0;
    uint32_t bytes_total = 0;
};

struct FlashOptions {
    uint32_t scan_ms = 600000;   // how long to wait for the target on attempt 1 (arm timeout)
    uint16_t prn = dfu::kDefaultPrn;  // capped at dfu::kMaxSafePrn unless force_prn
    bool force_prn = false;
    uint32_t start_settle_ms = 500;
    uint32_t prn_timeout_ms = 5000;
    uint16_t max_packet = 0;     // 0 = automatic; else cap (e.g. 20 for testing)
    ConnParams conn{};           // interval for attempt 1; retries may lengthen it
    uint8_t attempts = 3;        // total attempts (SPEC.md §6.5)
    /// Called right before each attempt's transfer starts, i.e. before START
    /// may erase the target (used for the job marker, SPEC.md §7.3).
    std::function<void(uint8_t try_number)> before_transfer;
    /// Called once per flash(), when the target is first seen and before
    /// connecting: the radio-handover point (SPEC.md §7.1).
    std::function<void()> on_found;
    /// Phase changes and stream progress. Runs on the flashing task.
    std::function<void(const FlashProgress&)> on_progress;
};

struct AttemptSummary {
    uint8_t number = 0;
    dfu::AttemptParams params;   // as used (PRN after the cap)
    dfu::FailureKind failure = dfu::FailureKind::None;
    std::string error;           // empty on success
    dfu::TransferResult transfer;
    bool reached_bootloader = false;
    bool ran_transfer = false;   // `transfer` is meaningful
    bool sys_reset_sent = false; // SYS_RESET sent after this attempt's failure
};

struct FlashReport {
    bool ok = false;
    std::string error;                 // from the last attempt
    dfu::FailureKind failure = dfu::FailureKind::None;
    LinkInfo link;                     // last attempt that reached the bootloader
    dfu::TransferResult transfer;      // last attempt that actually ran a transfer
    std::vector<AttemptSummary> attempts;
    bool target_erased = false;        // any attempt got as far as START
    bool sys_reset_after_failure = false;  // the last attempt sent SYS_RESET after failing
    bool aborted = false;              // request_abort() ended the run
    uint32_t total_ms = 0;
};

/// Run control (SPEC.md §7.3; console Ctrl+C, API abort, run deadline).
/// request_abort() may be called from any task or timer. scan / probe / flash /
/// reset_bootloader stop at their next wait point: target scan, retry
/// back-off, notification wait or the next packet, each checked at least every
/// 250 ms. Exceptions:
///  - a blocking NimBLE call such as connect (10 s timeout) finishes first;
///  - a transfer whose last packet is already sent completes;
///  - once connected to the bootloader, an aborted probe/flash still sends
///    SYS_RESET so the session is cleared and the target isn't left in DFU
///    mode; an abort while looking for the bootloader right after the
///    buttonless jump spends up to 5 s more finding it to send SYS_RESET.
/// The flag stays set until clear_abort(), which the caller does before the
/// next run.
void request_abort();
void clear_abort();
bool abort_requested();

/// esp_timer time (us) of the last sign of life from a blocking wait inside
/// flash()/probe()/scan(): waits beat at least every 250 ms, so a value that
/// stops moving for long means the task is wedged (the orchestrator's stall
/// watchdog, SPEC.md §7.3).
int64_t last_activity_us();

/// Flashes the image with automatic retries (SPEC.md §6.5). Each attempt checks
/// the device type and SoftDevice requirement against the bootloader before
/// START; mismatches stop immediately and SYS_RESET returns an untouched target
/// to its app. After any failure with the bootloader still connected, SYS_RESET
/// clears the session so the next attempt can START. A stream overrun or link
/// loss makes the next attempt more conservative (dfu::degrade). On
/// request_abort() it stops with dfu::FailureKind::Aborted and does not retry;
/// an abort noticed before START never erases the target.
FlashReport flash(const Mac& app, dfu::IImageSource& image, uint32_t image_size,
                  std::span<const uint8_t> init_packet, const dfu::InitPacket& init,
                  const FlashOptions& opts);

/// Connects to the target's bootloader (MAC or MAC+1) and sends SYS_RESET.
/// For recovering a bootloader left mid-session by an aborted run.
bool reset_bootloader(const Mac& app, uint32_t scan_ms, std::string& error);

}  // namespace dfu32::ble
