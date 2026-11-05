// SPDX-License-Identifier: MIT
//
// Target settings (SPEC.md §9.1, v1 cap = 1 target). Pure struct + validation;
// NVS persistence lives in the `config` component.
#pragma once

#include <cstdint>
#include <string>

namespace dfu32 {

struct TargetConfig {
    std::string id;            // slug [a-z0-9-], 1-31 chars; "" = not set yet
    std::string name;          // display name, <= 63
    std::string board;         // matched against manifest.boards, <= 31
    std::string mac;           // app MAC "AA:BB:CC:DD:EE:FF" (MSB first), "" = not set
    uint8_t addr_type = 1;     // 0 public, 1 random
    std::string manifest_url;  // http://..., <= 255
    uint32_t max_app_size = 0; // 0 = default for the image's device type

    /// id, or "target" when none is set (the staging header needs one).
    std::string effective_id() const { return id.empty() ? std::string("target") : id; }
};

/// Field validators; each returns an empty string when the value is acceptable
/// and otherwise a short reason. Empty values are allowed (= unset).
std::string validate_target_id(const std::string& id);
std::string validate_board(const std::string& board);
std::string validate_mac(const std::string& mac);  // also accepts lowercase; see normalize_mac
std::string validate_manifest_url(const std::string& url);

/// Uppercases a valid MAC; returns "" if invalid.
std::string normalize_mac(const std::string& mac);

/// All fields at once; the first problem, or "".
std::string validate(const TargetConfig& t);

}  // namespace dfu32
