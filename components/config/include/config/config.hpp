// SPDX-License-Identifier: MIT
//
// NVS persistence for device and target settings (SPEC.md §9.1), namespace
// "dfu32". Target 0 is a single JSON record (`tgt0`), so saving it is atomic.
// A target set with an M1 build (tgt0_mac / tgt0_type) is read until the
// first save replaces it.
#pragma once

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

}  // namespace dfu32::config
