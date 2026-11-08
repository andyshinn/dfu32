// SPDX-License-Identifier: MIT
//
// Improv BLE (SPEC.md §9.2): a NimBLE GATT server that takes WiFi settings
// from a phone or browser (Home Assistant, the Improv web page). It runs only
// while DFU32 has no network stored, for 5 minutes after a 2 s button press,
// or when started from the console. While it runs it holds the device-wide
// BLE lease, so no scan or flash can start (409 Busy, holder "improv").
#pragma once

#include <cstdint>
#include <span>

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
