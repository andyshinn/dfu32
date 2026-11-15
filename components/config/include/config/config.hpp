// SPDX-License-Identifier: MIT
//
// NVS persistence for device and target settings (SPEC.md §9.1), namespace
// "dfu32". Target 0 is a single JSON record (`tgt0`), so saving it is atomic.
// A target set with an M1 build (tgt0_mac / tgt0_type) is read until the
// first save replaces it.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "dfu32_core/api_core.hpp"
#include "dfu32_core/target_config.hpp"

namespace dfu32::config {

/// Loads target 0. Missing keys leave the defaults; returns false only if NVS
/// can't be opened at all.
bool load_target(TargetConfig& out);

/// Stores target 0 in one NVS write (validate() it first). On failure the
/// previous record is left as it was.
bool save_target(const TargetConfig& t);

/// Removes all target-0 keys.
bool erase_target();

/// Device-level settings (SPEC.md §9.1): keys hostname, ntp_server,
/// wifi_off_dfu, dfu_deadline, and the AP fallback's ap_enabled, ap_password,
/// ap_delay, ap_idle (§9.5). net reads hostname/ntp_server at boot, so changes
/// to those apply after a reboot. load_device sanitises the AP keys
/// (ap::sanitize_loaded): a bad delay or idle limit loads as the default, and a bad
/// password turns AP mode off rather than opening the AP.
bool load_device(api::DeviceConfig& out);
bool save_device(const api::DeviceConfig& c);

/// Read-modify-write of the device settings under one mutex (console `ap` and
/// PUT /config can't lose each other's change). edit returns "" or a reason;
/// then validate(); then save. Returns "" or the reason ("NVS write failed").
/// Nothing is saved on a reason. *out gets the saved settings. While the stored
/// AP password is invalid, an edit that doesn't set it keeps it in NVS and may
/// not turn AP mode on (§9.5); the edit then sees a stand-in password.
std::string update_device(const std::function<std::string(api::DeviceConfig&)>& edit,
                          api::DeviceConfig* out = nullptr);

/// SHA-256 of the API token (SPEC.md §9.3). False when none is set (the API is open).
bool load_token_hash(std::array<uint8_t, 32>& out);
bool save_token_hash(const std::array<uint8_t, 32>& hash);
bool clear_token();

/// Factory reset (SPEC.md §9.3): erases the whole `dfu32` namespace (target,
/// device settings including the AP settings, token hash). WiFi credentials
/// are the WiFi driver's and are cleared separately (net::factory_reset(),
/// called first so the AP can't come up before the reboot); the staging
/// partition and run log are kept.
bool erase_all();

}  // namespace dfu32::config
