// SPDX-License-Identifier: MIT
#pragma once

namespace dfu32 {

/// Starts the USB-serial bench console (REPL task) and returns.
/// `on_improv_start` runs after `improv start` opens Improv BLE: main then
/// keeps a phone joining the access point from closing that window (§9.5).
void start_console(void (*on_improv_start)() = nullptr);

}  // namespace dfu32
