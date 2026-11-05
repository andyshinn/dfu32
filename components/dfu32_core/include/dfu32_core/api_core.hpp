// SPDX-License-Identifier: MIT
//
// Pure helpers behind the HTTP API (SPEC.md §9.3, §10.1): route matching,
// bearer-token handling and the auth throttle, device-level settings, and
// the BLE scan result view. The esp_http_server glue lives in components/api.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dfu32::json {
class ObjectWriter;
}

namespace dfu32::api {

// ---- routes ----------------------------------------------------------------------

enum class Method : uint8_t { Get, Put, Post, Delete, Other };

enum class Route : uint8_t {
    NotFound,
    Status,        // GET  /api/v1/status
    Claim,         // POST /api/v1/claim
    Targets,       // GET  /api/v1/targets
    Target,        // GET/PUT/DELETE /api/v1/targets/{id}
    TargetCheck,   // POST /api/v1/targets/{id}/check
    TargetStage,   // POST /api/v1/targets/{id}/stage
    TargetFlash,   // POST /api/v1/targets/{id}/flash
    TargetRuns,    // GET  /api/v1/targets/{id}/runs
    JobAbort,      // POST /api/v1/jobs/current/abort
    Run,           // GET  /api/v1/runs/{seq}
    Staging,       // GET/DELETE /api/v1/staging
    BleScan,       // POST /api/v1/ble/scan
    Logs,          // GET  /api/v1/logs?since=
    Config,        // GET/PUT /api/v1/config
    Reboot,        // POST /api/v1/system/reboot
    TokenRotate,   // POST /api/v1/system/token/rotate
    DebugFault,    // POST /api/v1/debug/fault (CONFIG_DFU32_DEBUG_ENDPOINTS only)
};

struct RouteMatch {
    Route route = Route::NotFound;
    std::string id;     // {id} for target routes (not validated here)
    uint32_t seq = 0;   // {seq} for Run
};

/// Matches a request URI (any "?query" is ignored; one trailing '/' is ok).
RouteMatch match_route(std::string_view uri);
bool method_allowed(Route r, Method m) noexcept;
/// Routes that work without a token (and while unclaimed).
bool is_public(Route r) noexcept;
/// The value of `key` in the URI's query string, or "" (no %-decoding:
/// the API's query values are numbers).
std::string_view query_param(std::string_view uri, std::string_view key);

// ---- auth --------------------------------------------------------------------------

/// RFC 4648 §5 base64url, no padding (the claim token's text form).
std::string base64url_encode(std::span<const uint8_t> data);

/// Compares in time that depends only on the lengths.
bool ct_equal(std::span<const uint8_t> a, std::span<const uint8_t> b) noexcept;

/// The token from an "Authorization: Bearer <token>" header value, or "".
std::string_view bearer_token(std::string_view header) noexcept;

/// SPEC.md §10.1: after 5 failed attempts within a minute, for the next
/// 60 s at most one token is checked per second; other authenticated
/// requests are refused (429, Retry-After: 1) without sleeping, so the
/// single server thread never stalls.
class AuthThrottle {
public:
    static constexpr uint32_t kMaxFailures = 5;
    static constexpr uint64_t kWindowMs = 60000;
    static constexpr uint64_t kPenaltyMs = 60000;
    static constexpr uint64_t kSlotMs = 1000;

    void record_failure(uint64_t now_ms) noexcept;
    /// Whether a token may be checked at `now_ms`. Outside a penalty always
    /// true; during one, true once per kSlotMs (and that slot is used up).
    bool try_attempt(uint64_t now_ms) noexcept;
    bool penalised(uint64_t now_ms) const noexcept { return now_ms < penalty_until_; }
    uint32_t failures() const noexcept { return failures_; }

private:
    uint32_t failures_ = 0;
    uint64_t window_start_ = 0;
    uint64_t penalty_until_ = 0;
    uint64_t next_slot_ = 0;
};

// ---- device settings (SPEC.md §9.1) -----------------------------------------------

struct DeviceConfig {
    std::string hostname;         // "" = default dfu32-xxxx
    std::string ntp_server;       // "" = default pool.ntp.org
    bool wifi_off_dfu = true;     // default for a flash request's wifi_off
    uint32_t dfu_deadline_max_s = 1200;  // cap on the §7.3 run deadline
};

inline constexpr uint32_t kMinDeadlineMaxS = 120;
inline constexpr uint32_t kMaxDeadlineMaxS = 7200;

/// Host name or dotted IPv4: 1-63 of [A-Za-z0-9.-], labels not starting or
/// ending with '-'. Empty = default.
std::string validate_ntp_server(const std::string& s);
std::string validate(const DeviceConfig& c);
void write_json(json::ObjectWriter& w, const DeviceConfig& c);
/// PUT /api/v1/config: merges present fields (unknown ones are refused) and
/// validates. "" on success, else the reason (`c` unchanged).
std::string apply_json(std::string_view body, DeviceConfig& c);

// ---- BLE scan view (POST /api/v1/ble/scan) ----------------------------------------

/// POST /api/v1/ble/scan body: {"duration_s":1-30 (default 10),"dfu_only":bool}.
struct ScanRequest {
    uint32_t duration_s = 10;
    bool dfu_only = false;
};
std::string parse_scan_request(std::string_view body, ScanRequest& out);

/// POST /api/v1/debug/fault body (fault injection, SPEC.md §14.2):
/// {"restore_fail":bool,"deadline_s":0-3600,"stall_s":0-600}.
struct FaultRequest {
    bool restore_fail = false;
    uint32_t deadline_s = 0;
    uint32_t stall_s = 0;
};
std::string parse_fault_request(std::string_view body, FaultRequest& out);

/// What the advertiser probably is, from its name and services. Only a
/// connection (Version characteristic) confirms it, so this is a hint.
/// "app_buttonless" (MeshCore's `start ota` advert, name ends in "_OTA"),
/// "bootloader" (name contains "DFU", or the legacy DFU service is
/// advertised), or nullptr.
const char* classify_dfu(std::string_view name, bool dfu_service) noexcept;

/// Bootloader address after a 0xA8 reset: last octet + 1 (wraps, no carry).
/// Input and output MSB first ("F4:1C:36:A7:33:95" -> "F4:1C:36:A7:33:96");
/// "" if `mac` is not a valid MAC.
std::string mac_plus_one(const std::string& mac);

/// `addr` is the target's app MAC or MAC+1 (case-insensitive).
bool matches_target(const std::string& addr, const std::string& target_mac);

}  // namespace dfu32::api
