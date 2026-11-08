// SPDX-License-Identifier: MIT
//
// Improv BLE (SPEC.md §9.2): a NimBLE GATT server that takes WiFi settings
// from a phone or browser (Home Assistant, the Improv web page). It runs only
// while DFU32 has no network stored, for 5 minutes after a 2 s button press,
// or when started from the console. While it runs it holds the device-wide
// BLE lease, so no scan or flash can start (409 Busy, holder "improv").
#pragma once

#include <cstdint>

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
/// while already open restarts its 5 minutes.
bool open(Window why);

/// Stops advertising, drops a connected client and releases the lease.
void close();

bool active();
State state();

/// True for 10 s after an Improv "identify" request (the LED blinks white).
bool identifying();

}  // namespace dfu32::improv
