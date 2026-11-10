// SPDX-License-Identifier: MIT
//
// NVS persistence for device and target settings (SPEC.md §9.1), namespace
// "dfu32". Target 0 is a single JSON record (`tgt0`), so saving it is atomic.
// A target set with an M1 build (tgt0_mac / tgt0_type) is read until the
// first save replaces it.
#pragma once

#include <array>
#include <cstdint>

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
/// wifi_off_dfu, dfu_deadline. net reads hostname/ntp_server at boot, so
/// changes to those apply after a reboot.
bool load_device(api::DeviceConfig& out);
bool save_device(const api::DeviceConfig& c);

/// SHA-256 of the API token (SPEC.md §9.3). False when none is set (the API is open).
bool load_token_hash(std::array<uint8_t, 32>& out);
bool save_token_hash(const std::array<uint8_t, 32>& hash);
bool clear_token();

/// Factory reset (SPEC.md §9.3): erases the whole `dfu32` namespace (target,
/// device settings, token hash). WiFi credentials are the WiFi driver's and
/// are cleared separately; the staging partition and run log are kept.
bool erase_all();

}  // namespace dfu32::config
