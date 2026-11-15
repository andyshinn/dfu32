// SPDX-License-Identifier: MIT
//
// WiFi access-point fallback (SPEC.md §9.5): the AP settings and their
// validation, the console `ap` grammar, the open-AP warning, and the policy
// that decides when DFU32's own access point is up. Pure, so the rule table,
// its timing boundaries and the station-retry pause are host-tested; the
// esp_wifi glue lives in components/net.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dfu32::ap {

// One constant per line: tools/test_ui.py and tools/test_dfu32_web_manifest.py
// read the delay and password limits from this header with a regex.
inline constexpr uint32_t kDefaultDelayS = 120;
inline constexpr uint32_t kMinDelayS = 30;
inline constexpr uint32_t kMaxDelayS = 1800;
inline constexpr uint32_t kMinPasswordLen = 8;
inline constexpr uint32_t kMaxPasswordLen = 63;
inline constexpr uint8_t kMaxClients = 4;
// esp_wifi_set_inactive_time(WIFI_IF_AP): a phone that left without a deauth
// can't hold the station-retry pause for longer than this.
inline constexpr uint16_t kInactiveS = 60;
// Station has an IP and the AP has had no client for this long -> AP down. Covers
// the phone dropping and rejoining when the AP follows the station's channel.
inline constexpr uint64_t kDownGraceMs = 30000;
// Explicit join (set_credentials) or restore(): station retries run even with
// clients present. Longer than the 30 s Improv attempt and the §7.3 60 s verify.
inline constexpr uint64_t kJoinWindowMs = 65000;
// Strict pause (decided): no automatic station attempt at all while a phone is
// on the AP. 0 keeps probe_now always false.
inline constexpr uint64_t kPausedProbeMs = 0;
inline constexpr uint64_t kBringUpRetryMs = 10000;
// AP settings changes apply this long after the reply (= job.cpp kReplyGraceMs),
// so a phone on the AP being changed still gets its 200.
inline constexpr uint32_t kApplyGraceMs = 1500;
inline constexpr uint64_t kNever = UINT64_MAX;
inline constexpr uint8_t kIp[4] = {192, 168, 4, 1};
inline constexpr const char* kIpText = "192.168.4.1";
inline constexpr const char* kUrl = "http://192.168.4.1/";

/// "" = OK (empty = open AP). Else the reason: 8-63 printable ASCII (0x20-0x7E);
/// a 64-hex raw PSK is not accepted.
std::string validate_ap_password(std::string_view pw);
/// "" = OK, else "ap_delay_s must be 30-1800".
std::string validate_ap_delay(uint64_t s);
/// Station credentials (console `wifi`, PUT /api/v1/wifi): SSID 1-32 bytes; password
/// empty, 8-63 printable ASCII, or exactly 64 hex digits (a raw PSK).
std::string validate_wifi_credentials(std::string_view ssid, std::string_view pw);

/// NVS load sanitising: an out-of-range delay becomes kDefaultDelayS (delay_reset); an invalid
/// password turns `enabled` off and clears `password` (password_invalid). Never yields an open AP
/// from a bad value.
struct LoadFixups {
    bool password_invalid = false;
    bool delay_reset = false;
};
LoadFixups sanitize_loaded(bool& enabled, std::string& password, uint32_t& delay_s);

/// AP channel when the station is idle: last_sta_primary if it is within the country's channel range
/// [schan, schan+nchan-1] and <= 14 (2.4 GHz), else schan (or 1 if schan is 0).
uint8_t ap_channel(uint8_t last_sta_primary, uint8_t country_schan, uint8_t country_nchan) noexcept;

/// The open-AP-without-token warning: nullptr unless enabled && open && !claimed. `active` picks the
/// text ("...is up and open..." vs "...whenever the access point comes up...").
const char* ap_warning(bool enabled, bool open, bool claimed, bool active) noexcept;

/// Friendly reason for a failed station join (wifi_err_reason_t): 2, 14, 15, 202, 204 -> "wrong password";
/// 201, 210, 211, 212 -> "network not found"; 0 -> nullptr; anything else -> "could not connect".
const char* describe_sta_reason(uint8_t reason) noexcept;

// ---- console `ap` grammar ---------------------------------------------------------

struct Command {
    enum class Kind : uint8_t { Status, On, Off, SetPassword, ClearPassword, Delay, Usage, Invalid };
    Kind kind = Kind::Status;
    std::string password;   // SetPassword
    uint32_t delay_s = 0;   // Delay
    std::string error;      // Invalid: reason printed as "Not saved: <error>"
};
/// argv[0] == "ap". Usage for unknown subcommands, missing values and extra arguments; Invalid for bad
/// values. Delay is parsed with the same rules as parse_u32 (dfu32_core/target_config.hpp).
Command parse_command(int argc, const char* const* argv);

// ---- fallback policy (SPEC.md §9.5 "When the AP is up") ----------------------------

struct Settings {
    bool enabled = false;
    uint32_t delay_s = kDefaultDelayS;
};
struct Facts {
    bool stopped = false;       // net::stop() in force
    bool provisioned = false;   // a station SSID is stored
    bool sta_has_ip = false;
    bool ap_up = false;         // driver mode includes the AP (source of truth)
    uint8_t clients = 0;        // re-read from esp_wifi_ap_get_sta_list; 0 when !ap_up
};
enum class Action : uint8_t { None, BringUp, TakeDown };
enum class Why : uint8_t { Stopped, Disabled, NoNetworkStored, ClientsPresent, StationUpGrace, StationUp, StationDown, Waiting };
struct Decision {
    bool want_up = false;
    Action action = Action::None;
    bool pause_sta = false;          // no automatic station retries
    bool probe_now = false;          // only when kPausedProbeMs > 0 (never, as decided)
    uint64_t next_eval_ms = kNever;  // > now when not kNever
    int64_t up_in_ms = -1;           // standby countdown, -1 = not pending
    Why why = Why::Disabled;
};

/// The one AP decision. net calls step() whenever something changes and again
/// at next_eval_ms, the next instant its answer could change by itself.
class Controller {
public:
    void boot(uint64_t now_ms) noexcept;                // "station without IP since" = now
    void ip_lost(uint64_t now_ms) noexcept;             // starts the clock if it isn't running
    void ip_got() noexcept;                             // stops the clock
    void stopped(bool had_ip) noexcept;                 // remembers had_ip for restored()
    void restored(uint64_t now_ms) noexcept;            // clock = now if it had an IP at stop, else unchanged
    void explicit_join(uint64_t now_ms) noexcept;       // join window until now + kJoinWindowMs
    void probed(uint64_t now_ms) noexcept;
    void bringup_failed(uint64_t now_ms) noexcept;      // BringUp suppressed until now + kBringUpRetryMs
    /// Updates the grace bookkeeping (down_ok_since), resyncs the "without an IP since" clock with
    /// facts.sta_has_ip unless stopped, and returns the decision. Rules in SPEC §9.5 order:
    /// stopped -> want_up = facts.ap_up, None, no pause, kNever; !enabled -> down; !provisioned -> up;
    /// ap_up && clients>0 -> up; sta_has_ip && ap_up -> up until kDownGraceMs with 0 clients;
    /// sta_has_ip && !ap_up -> down; !sta_has_ip -> ap_up || now - since >= delay.
    /// pause_sta = !stopped && enabled && ap_up && clients>0 && !sta_has_ip && provisioned && !join_window_open(now).
    Decision step(const Settings& s, const Facts& f, uint64_t now_ms) noexcept;
    bool join_window_open(uint64_t now_ms) const noexcept;
    uint64_t sta_down_since_ms() const noexcept;        // kNever while the station has an IP

private:
    uint64_t sta_down_since_ms_ = kNever, down_ok_since_ms_ = kNever, join_until_ms_ = 0,
             last_probe_ms_ = 0, bringup_retry_at_ms_ = 0;
    bool had_ip_at_stop_ = false;
};

}  // namespace dfu32::ap
