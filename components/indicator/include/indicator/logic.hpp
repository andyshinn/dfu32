// SPDX-License-Identifier: MIT
//
// What the status LEDs show and what a button hold does (SPEC.md §9.3,
// §13.4). Pure, host-tested; components/indicator drives the NeoPixel and the
// red LED through espressif/led_indicator and reads the button through
// espressif/button.
#pragma once

#include <cstdint>

#include "dfu32_core/job.hpp"

namespace dfu32::indicator {

/// NeoPixel patterns. The order is also the blink-list order in the driver.
enum class Pattern : uint8_t {
    Off,
    Idle,       // dim green
    Offline,    // dim yellow, slow blink: WiFi configured but not connected
    Improv,     // white, slow blink: Improv BLE waiting for WiFi settings
    Fetching,   // blue breathe: fetching / verifying / staging
    Awaiting,   // cyan blink: armed, run `start ota`
    Transfer,   // magenta, brightness tracking progress
    Success,    // solid green, 10 s
    Failed,     // solid red: the last job failed
    Erased,     // red fast blink: the target's app is erased and not reflashed
    Identify,   // white fast blink: Improv "identify"
    Hold2,      // amber slow blink: release now opens the Improv window
    Hold5,      // amber fast blink: release now clears the API token
    Hold15,     // solid amber: release now factory-resets
    Count,
};

enum class RedLed : uint8_t { Off, On, Blink };

/// Remembers outcomes across jobs: success shows for 10 s, a failure until
/// the next job, an erased target until a later flash succeeds.
class Tracker {
public:
    static constexpr uint32_t kSuccessShowS = 10;
    void update(const job::Snapshot& s, uint32_t now_s);
    bool erased_pending() const noexcept { return erased_; }
    bool failed() const noexcept { return failed_; }
    bool success_showing(uint32_t now_s) const noexcept { return now_s < success_until_; }

private:
    uint32_t seen_id_ = 0;
    bool seen_terminal_ = false;
    bool erased_ = false;
    bool failed_ = false;
    uint32_t success_until_ = 0;
};

struct Inputs {
    job::State job_state = job::State::Idle;
    uint32_t bytes_done = 0;
    uint32_t bytes_total = 0;
    bool erased_pending = false;
    bool failed = false;
    bool success_showing = false;
    bool provisioned = false;     // WiFi credentials stored
    bool wifi_connected = false;
    bool wifi_stopped = false;    // stopped on purpose for a transfer
    bool improv_active = false;
    bool identify = false;
    uint8_t hold_level = 0;       // 0, 2, 5 or 15 (seconds held so far)
};

struct Output {
    Pattern pattern = Pattern::Off;
    uint8_t brightness = 255;     // used by Transfer
    RedLed red = RedLed::Off;
    bool operator==(const Output&) const = default;
};

/// Highest priority first: button feedback, identify, a running job (its
/// progress beats stale outcomes), Improv, erased target, failure, success,
/// WiFi down, idle.
Output decide(const Inputs& in);

enum class ButtonAction : uint8_t { None, ImprovWindow, ClearToken, FactoryReset };

/// What releasing the button after `held_ms` does: 2-5 s opens Improv BLE for
/// 5 minutes, 5-15 s clears the API token, 15 s or more factory-resets.
ButtonAction classify_hold(uint32_t held_ms) noexcept;
/// The feedback level while still held (0, 2, 5, 15).
uint8_t hold_level(uint32_t held_ms) noexcept;

}  // namespace dfu32::indicator
