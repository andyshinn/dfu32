// SPDX-License-Identifier: MIT
//
// NVS persistence for device and target settings (SPEC.md §9.1), namespace
// "dfu32". Target 0 keeps the M1 keys (tgt0_mac, tgt0_type) so a target set
// with an older build survives the update.
#pragma once

#include "dfu32_core/target_config.hpp"

namespace dfu32::config {

/// Loads target 0. Missing keys leave the defaults; returns false only if NVS
/// can't be opened at all.
bool load_target(TargetConfig& out);

/// Stores every field of target 0 (validate() it first).
bool save_target(const TargetConfig& t);

/// Removes all target-0 keys.
bool erase_target();

}  // namespace dfu32::config
