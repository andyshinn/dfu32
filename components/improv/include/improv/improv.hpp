// SPDX-License-Identifier: MIT
//
// Improv BLE (SPEC.md §9.2): a NimBLE GATT server that takes WiFi settings
// from a phone or browser (Home Assistant, the Improv web page). It runs only
// while DFU32 has no network stored, for 5 minutes after a 2 s button press,
// or when started from the console. While it runs it holds the device-wide
// BLE lease, so no scan or flash can start (409 Busy, holder "improv"). The
// window opened at boot closes when the first phone joins DFU32's access
// point (SPEC.md §9.5), so its lease doesn't lock out the AP operator.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "improv/core.hpp"

namespace dfu32::improv {

enum class Window : uint8_t {
    Unprovisioned,  // until provisioned (or closed from the console)
    Button,         // 5 minutes
    Console,        // until provisioned or `improv stop`
};

/// Registers the GATT service. Call once, after ble::init() and net::init().
bool init();

/// Starts advertising. False if Improv BLE is disabled in Kconfig or the BLE
/// lease is held (a job, scan or console flash is running). A Button window
/// while already open (re)arms its 5 minutes. Waits for the Improv task.
bool open(Window why);

/// Stops advertising, drops a connected client, puts the previous network
/// back if an attempt was still connecting, and releases the lease. Returns
/// whether it was open (the close itself happens on the Improv task).
bool close();

struct Snapshot {
    bool active = false;
    State state = State::Stopped;
};
/// Both values read together.
Snapshot snapshot();
bool active();

/// True for 10 s after an Improv "identify" request (the LED blinks white).
bool identifying();

}  // namespace dfu32::improv

// ---- Improv Serial (SPEC.md §9.2) -------------------------------------------------------
//
// Always on, on the console port: the console reader hands it frames, and it
// answers through `write`. It stays on after provisioning, so the browser
// installer can recognise DFU32 and change its WiFi.

namespace dfu32::improv {

/// Sends one encoded frame (from encode_serial_frame) to the console port.
using SerialWriteFn = void (*)(std::span<const uint8_t> frame);

/// Starts the Improv Serial task. Call once, after net::init().
bool serial_start(SerialWriteFn write);

/// A complete frame from the console reader; copied, never blocks.
void serial_deliver(uint8_t type, std::span<const uint8_t> data);

/// The reader dropped a frame (bad checksum, too long): answers INVALID_RPC,
/// so the client doesn't wait for its timeout.
void serial_invalid_frame();

}  // namespace dfu32::improv

// ---- Station setup from the web UI (SPEC.md §9.5) ---------------------------------------
//
// PUT /api/v1/wifi runs the same kind of attempt as Improv, on the Improv
// Serial task, so there is at most one attempt across Improv BLE, Improv
// Serial and the web.

namespace dfu32::improv {

enum class WebAttemptState : uint8_t { None, Pending, Joining, Joined, Failed };

struct WebAttempt {
    WebAttemptState state = WebAttemptState::None;
    uint8_t reason = 0;   // wifi_err_reason_t when Failed by timeout (0 if none recorded)
    bool refused = false; // Failed because the lease was held or another attempt ran when it was due
};

/// Queues an attempt on the Improv Serial task that starts `delay_ms` later,
/// so the 202 reaches the phone before the AP follows the station to another
/// channel. Like an Improv attempt: lease "wifi-setup", 30 s for an IP, the
/// previous network put back on failure, and an open Improv BLE window closed
/// on success. Copies the passphrase and wipes it once used. False if one is
/// already pending or running, or the task isn't up.
bool web_connect(const std::string& ssid, const std::string& password, uint32_t delay_ms = 1500);

/// The last web attempt, kept in RAM until the next one so a phone that
/// rejoined the AP can still read the outcome (/status device.wifi.attempt).
WebAttempt web_attempt();

/// True while any Improv BLE, Improv Serial or web attempt runs, or a web
/// attempt is pending: PUT /wifi and the WiFi scan answer 409 then.
bool attempt_running();

}  // namespace dfu32::improv
