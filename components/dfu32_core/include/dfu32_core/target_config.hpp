// SPDX-License-Identifier: MIT
//
// Target settings (SPEC.md §9.1, v1 cap = 1 target). Pure struct + validation;
// NVS persistence lives in the `config` component.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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

/// 0 (= default) or a multiple of 4 between 4 KiB and 16 MiB.
std::string validate_max_app_size(uint32_t bytes);

/// DHCP/mDNS host label: 1-31 of [a-z0-9-], not starting or ending with '-'.
std::string validate_hostname(const std::string& name);

/// Strict unsigned parse: decimal or 0x-hex, the whole string, no overflow.
bool parse_u32(std::string_view text, uint32_t& out) noexcept;

/// Uppercases a valid MAC; returns "" if invalid.
std::string normalize_mac(const std::string& mac);

/// All fields at once; the first problem, or "".
std::string validate(const TargetConfig& t);

/// JSON form, used as the single atomic NVS record (and by the M3b API).
std::string to_json(const TargetConfig& t);
/// Parses to_json() output; unknown keys are ignored, missing ones keep
/// their defaults. Returns false on malformed JSON or wrongly typed fields.
bool from_json(std::string_view json, TargetConfig& out);

}  // namespace dfu32
