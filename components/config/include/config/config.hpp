// SPDX-License-Identifier: MIT
//
// NVS persistence for device and target settings (SPEC.md §9.1), namespace
// "dfu32". Each target is one JSON record in its own slot (`tgt0`..`tgt9`), so
// saving a target is a single atomic write; `tgt_def` names the default
// target. A target set with an M1 build (tgt0_mac / tgt0_type) is read as
// slot 0 until the first save replaces it. The logic (slots, the cap, the
// default) is dfu32::TargetTable; this file only reads and writes NVS.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "dfu32_core/api_core.hpp"
#include "dfu32_core/target_config.hpp"
#include "dfu32_core/target_table.hpp"

namespace dfu32::config {

/// The build's target cap, CONFIG_DFU32_MAX_TARGETS (1..kMaxTargetSlots).
size_t max_targets();

/// Loads every stored target and the default into `out` (capped at
/// max_targets() for new targets; every stored slot is read). Records with
/// nothing set, corrupt records and duplicate ids are skipped with a log
/// line; an invalid field takes its default (sanitize()). Returns false only
/// if NVS can't be opened at all.
bool load_targets(TargetTable& out);

/// Persists the writes a TargetTable mutation returned, in order: the record
/// in `w.save_slot` (from `table`), the erase of `w.erase_slot`, the
/// default, then the skipped records in `w.purge_slots`. Stops at the first failure and returns false; a failed write
/// leaves that key as it was.
bool commit_targets(const TargetTable& table, const TargetWrites& w);

/// Convenience lookups (each loads the table). `found` says whether such a
/// target exists; they return false only when NVS can't be read.
bool load_target(const std::string& id, TargetConfig& out, bool& found);
bool load_default_target(TargetConfig& out, bool& found);

/// Device-level settings (SPEC.md §9.1): keys hostname, ntp_server,
/// wifi_off_dfu, dfu_deadline, and the AP fallback's ap_enabled, ap_password,
/// ap_delay (§9.5). net reads hostname/ntp_server at boot, so changes to
/// those apply after a reboot. load_device sanitises the AP keys
/// (ap::sanitize_loaded): a bad delay loads as the default, and a bad
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

/// Factory reset (SPEC.md §9.3): erases the whole `dfu32` namespace (targets,
/// device settings including the AP settings, token hash). WiFi credentials
/// are the WiFi driver's and are cleared separately (net::factory_reset(),
/// called first so the AP can't come up before the reboot); the staging
/// partition and run log are kept.
bool erase_all();

}  // namespace dfu32::config
