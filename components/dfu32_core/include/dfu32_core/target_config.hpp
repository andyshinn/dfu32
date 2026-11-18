// SPDX-License-Identifier: MIT
//
// Target settings (SPEC.md §9.1, v1 cap = 1 target). Pure struct + validation;
// NVS persistence lives in the `config` component.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "dfu_proto/retry.hpp"

namespace dfu32 {

/// Tuning-knob defaults and limits (SPEC.md §6, §9.1).
inline constexpr uint16_t kDefaultPrn = dfu::kDefaultPrn;  // 4
inline constexpr uint16_t kMaxPrn = dfu::kMaxSafePrn;      // 8
inline constexpr uint16_t kDefaultIntervalMs = dfu::kDefaultIntervalMs;  // 30
// 15 ms is the fastest interval the bench has seen work (docs/BENCH.md: 11.25 ms and
// 7.5 ms lost the link on OTAFIX, after the app was erased). The console's
// `--interval` still takes anything, for bench runs.
inline constexpr uint16_t kMinIntervalMs = 15;
inline constexpr uint16_t kMaxIntervalMs = 100;  // well inside the 2 s supervision timeout
inline constexpr uint16_t kDefaultMaxAttempts = 3;
inline constexpr uint16_t kMaxAttemptsLimit = 10;
inline constexpr uint16_t kDefaultArmTimeoutS = 600;
inline constexpr uint16_t kMinArmTimeoutS = 10;
inline constexpr uint16_t kMaxArmTimeoutS = 3600;
inline constexpr uint16_t kDefaultStartSettleMs = 500;
inline constexpr uint16_t kMaxStartSettleMs = 5000;
inline constexpr uint16_t kDefaultPrnTimeoutMs = 5000;
inline constexpr uint16_t kMinPrnTimeoutMs = 500;
inline constexpr uint16_t kMaxPrnTimeoutMs = 60000;

struct TargetConfig {
    std::string id;            // slug [a-z0-9-], 1-31 chars; "" = not set yet
    std::string name;          // display name, <= 63
    std::string board;         // the target's ID in the web UI, <= 31: compared with the image's
                               // (staged header board, manifest.boards) when both are set
    std::string mac;           // app MAC "AA:BB:CC:DD:EE:FF" (MSB first), "" = not set
    uint8_t addr_type = 1;     // 0 public, 1 random
    std::string manifest_url;  // http://..., <= 255
    uint32_t max_app_size = 0; // 0 = default for the image's device type

    // Tuning knobs (per target, SPEC.md §9.1).
    uint16_t prn = kDefaultPrn;                    // 1..8 (receipts acknowledge arrival, not flash writes)
    uint16_t interval_ms = kDefaultIntervalMs;     // 15..100, BLE connection interval for attempt 1
    uint16_t max_attempts = kDefaultMaxAttempts;   // 1..10
    uint16_t arm_timeout_s = kDefaultArmTimeoutS;  // 10..3600
    uint16_t start_settle_ms = kDefaultStartSettleMs;  // 0..5000
    uint16_t prn_timeout_ms = kDefaultPrnTimeoutMs;    // 500..60000

    // Updated by DFU32 after a successful flash; read-only over the API.
    std::string last_ok_version;
    uint64_t last_ok_at = 0;   // unix seconds (0 = unknown / clock not synced)

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

/// Resets every field that fails validation to its default (an invalid MAC
/// or URL becomes unset) and returns how many were reset. Used when loading
/// a stored record and before a PUT merge, so a record that breaks a newer
/// rule can still be loaded and repaired.
unsigned sanitize(TargetConfig& t);

namespace json {
class ObjectWriter;
}

/// JSON form, used as the single atomic NVS record and by the HTTP API.
std::string to_json(const TargetConfig& t);
void write_json(json::ObjectWriter& w, const TargetConfig& t);
/// Parses to_json() output; unknown keys are ignored, missing ones keep
/// their defaults. Returns false on malformed JSON or wrongly typed fields.
bool from_json(std::string_view json, TargetConfig& out);

/// PUT /api/v1/targets/{id}: merges the fields present in `body` into `t`
/// (a new target starts from defaults) and validates the result. `id` comes
/// from the path; a body "id" must match it. last_ok_* are read-only and
/// ignored, and so is "last_run", so a GET result can be sent back; so is
/// "default" (a boolean, which TargetTable::put_json acts on). Unknown fields
/// are refused.
/// Returns "" on success, otherwise the reason (and `t` is unchanged).
std::string apply_json(std::string_view body, const std::string& id, TargetConfig& t);

}  // namespace dfu32
