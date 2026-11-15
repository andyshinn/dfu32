// SPDX-License-Identifier: MIT
#include "dfu32_core/ap.hpp"

#include <algorithm>

#include "dfu32_core/target_config.hpp"

namespace dfu32::ap {

namespace {

bool printable(std::string_view s) noexcept {
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E) return false;  // IEEE 802.11i passphrase alphabet; refuses UTF-8 too
    }
    return true;
}

bool all_hex(std::string_view s) noexcept {
    for (const char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

// a + b, or kNever if that would wrap (callers compare it with "now").
uint64_t add_sat(uint64_t a, uint64_t b) noexcept { return a > kNever - b ? kNever : a + b; }

}  // namespace

std::string validate_ap_password(std::string_view pw) {
    if (pw.empty()) return {};
    // WPA2-Personal passphrase only (SPEC.md §9.5): the console and the installer
    // type it, so a 64-hex raw PSK would only be a way to get it wrong.
    if (pw.size() < kMinPasswordLen || pw.size() > kMaxPasswordLen || !printable(pw)) {
        return "ap_password must be 8-63 printable ASCII characters";
    }
    return {};
}

std::string validate_ap_delay(uint64_t s) {
    if (s < kMinDelayS || s > kMaxDelayS) return "ap_delay_s must be 30-1800";
    return {};
}

std::string validate_wifi_credentials(std::string_view ssid, std::string_view pw) {
    // An SSID is 1-32 arbitrary bytes (802.11); the driver's wifi_config_t holds 32.
    if (ssid.empty() || ssid.size() > 32) return "ssid must be 1-32 bytes";
    if (pw.empty()) return {};
    if (pw.size() == 64 && all_hex(pw)) return {};  // raw PSK, as a router's label may print it
    if (pw.size() < 8 || pw.size() > 63 || !printable(pw)) {
        return "password must be 8-63 printable ASCII characters or 64 hex digits";
    }
    return {};
}

LoadFixups sanitize_loaded(bool& enabled, std::string& password, uint32_t& delay_s) {
    LoadFixups f;
    if (!validate_ap_password(password).empty()) {
        // A corrupt value must never load as "open" (SPEC.md §9.5): AP mode stays
        // off until a valid password, or an explicit `ap password clear`, is stored.
        f.password_invalid = true;
        enabled = false;
        password.clear();
    }
    if (!validate_ap_delay(delay_s).empty()) {
        f.delay_reset = true;
        delay_s = kDefaultDelayS;
    }
    return f;
}

uint8_t ap_channel(uint8_t last_sta_primary, uint8_t country_schan, uint8_t country_nchan) noexcept {
    // Starting on the router's last channel means a router that comes back causes
    // no channel switch (and no phone drop) when the station joins it again.
    const uint32_t last = uint32_t{country_schan} + country_nchan;  // one past the country's range
    if (last_sta_primary != 0 && last_sta_primary <= 14 && last_sta_primary >= country_schan &&
        last_sta_primary < last) {
        return last_sta_primary;
    }
    return country_schan == 0 ? 1 : country_schan;
}

const char* ap_warning(bool enabled, bool open, bool claimed, bool active) noexcept {
    // Keyed on AP mode being on, not on the AP being up: a standby AP comes up
    // unattended, so the warning has to be seen before that happens.
    if (!enabled || !open || claimed) return nullptr;
    return active ? "The access point is up and open, and no API token is set: anyone in WiFi range can join it "
                    "and control DFU32, and can set a token that locks you out."
                  : "AP mode is on with an open access point and no API token: whenever the access point comes "
                    "up, anyone in WiFi range can join it and control DFU32, and can set a token that locks you "
                    "out.";
}

const char* describe_sta_reason(uint8_t reason) noexcept {
    switch (reason) {
        case 0: return nullptr;
        case 2:     // AUTH_EXPIRE
        case 14:    // MIC_FAILURE
        case 15:    // 4WAY_HANDSHAKE_TIMEOUT
        case 202:   // AUTH_FAIL
        case 204:   // HANDSHAKE_TIMEOUT
            return "wrong password";
        case 201:   // NO_AP_FOUND
        case 210:   // NO_AP_FOUND_W_COMPATIBLE_SECURITY
        case 211:   // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
        case 212:   // NO_AP_FOUND_IN_RSSI_THRESHOLD
            return "network not found";
        default: return "could not connect";
    }
}

// ---- console `ap` grammar ---------------------------------------------------------

Command parse_command(int argc, const char* const* argv) {
    Command c;
    if (argc <= 1 || argv == nullptr) return c;  // Status
    const std::string_view sub = argv[1];
    const std::string_view value = argc >= 3 ? std::string_view(argv[2]) : std::string_view();
    if ((sub == "on" || sub == "off") && argc == 2) {
        c.kind = sub == "on" ? Command::Kind::On : Command::Kind::Off;
    } else if (sub == "password" && argc == 3) {
        if (value == "clear") {
            c.kind = Command::Kind::ClearPassword;
        } else if (auto e = validate_ap_password(value); !e.empty()) {
            c.kind = Command::Kind::Invalid;
            c.error = std::move(e);
        } else {
            c.kind = Command::Kind::SetPassword;
            c.password = std::string(value);
        }
    } else if (sub == "delay" && argc == 3) {
        uint32_t s = 0;
        if (!parse_u32(value, s)) {
            c.kind = Command::Kind::Invalid;
            c.error = "not a number";
        } else if (auto e = validate_ap_delay(s); !e.empty()) {
            c.kind = Command::Kind::Invalid;
            c.error = std::move(e);
        } else {
            c.kind = Command::Kind::Delay;
            c.delay_s = s;
        }
    } else {
        c.kind = Command::Kind::Usage;
    }
    return c;
}

// ---- fallback policy --------------------------------------------------------------

void Controller::boot(uint64_t now_ms) noexcept { sta_down_since_ms_ = now_ms; }

void Controller::ip_lost(uint64_t now_ms) noexcept {
    // Only the first loss starts the clock: a disconnect event repeated by every
    // failed retry must not keep pushing the AP's start back.
    if (sta_down_since_ms_ == kNever) sta_down_since_ms_ = now_ms;
}

void Controller::ip_got() noexcept { sta_down_since_ms_ = kNever; }

void Controller::stopped(bool had_ip) noexcept { had_ip_at_stop_ = had_ip; }

void Controller::restored(uint64_t now_ms) noexcept {
    // The handover itself took the station's IP away, so the time stopped isn't
    // "the network is gone". A handover that began with the station already down
    // keeps the clock, so time spent stopped counts (SPEC.md §9.5).
    if (had_ip_at_stop_) sta_down_since_ms_ = now_ms;
}

void Controller::explicit_join(uint64_t now_ms) noexcept { join_until_ms_ = add_sat(now_ms, kJoinWindowMs); }

void Controller::probed(uint64_t now_ms) noexcept { last_probe_ms_ = now_ms; }

void Controller::bringup_failed(uint64_t now_ms) noexcept {
    bringup_retry_at_ms_ = add_sat(now_ms, kBringUpRetryMs);
}

bool Controller::join_window_open(uint64_t now_ms) const noexcept { return now_ms < join_until_ms_; }

uint64_t Controller::sta_down_since_ms() const noexcept { return sta_down_since_ms_; }

Decision Controller::step(const Settings& s, const Facts& f, uint64_t now_ms) noexcept {
    // The facts are the source of truth for the "without an IP since" clock: a
    // missed ip_lost() would otherwise keep the AP down for good, and a missed
    // ip_got() (or restored() landing after GOT_IP) would let the next loss bring
    // it up at once, skipping ap_delay_s. While stopped the clock is left alone
    // (restored() decides, SPEC.md §9.5).
    if (!f.stopped) {
        if (f.sta_has_ip) sta_down_since_ms_ = kNever;
        else if (sta_down_since_ms_ == kNever) sta_down_since_ms_ = now_ms;
    }

    // Grace bookkeeping: the 30 s runs from the moment rule 5 applies (a network
    // stored, the station has an IP, the AP has no client), and restarts whenever
    // a client associates.
    const bool grace_running =
        !f.stopped && s.enabled && f.provisioned && f.ap_up && f.sta_has_ip && f.clients == 0;
    if (!grace_running) down_ok_since_ms_ = kNever;
    else if (down_ok_since_ms_ == kNever) down_ok_since_ms_ = now_ms;

    Decision d;
    if (f.stopped) {
        // Radio handed over (§7.1): nothing touches the driver until restore().
        d.want_up = f.ap_up;
        d.why = Why::Stopped;
        return d;
    }

    if (!s.enabled) {
        d.why = Why::Disabled;  // an explicit off wins, even with phones on the AP
    } else if (!f.provisioned) {
        d.want_up = true;  // nothing to join: the AP is the only way in
        d.why = Why::NoNetworkStored;
    } else if (f.ap_up && f.clients > 0) {
        d.want_up = true;
        d.why = Why::ClientsPresent;
    } else if (f.sta_has_ip && f.ap_up) {
        const uint64_t elapsed = now_ms >= down_ok_since_ms_ ? now_ms - down_ok_since_ms_ : 0;
        d.want_up = elapsed < kDownGraceMs;
        d.why = Why::StationUpGrace;
        if (d.want_up) d.next_eval_ms = add_sat(down_ok_since_ms_, kDownGraceMs);
    } else if (f.sta_has_ip) {
        d.why = Why::StationUp;
    } else {
        // Once up, the AP stays up while the station has no IP; it only returns after
        // a fresh ap_delay_s without an IP, which is the hysteresis (SPEC.md §9.5).
        const uint64_t delay_ms = uint64_t{s.delay_s} * 1000;
        const uint64_t since = sta_down_since_ms_;
        const bool running = since != kNever;
        // since > now (clock skew) counts as "not yet": no underflow, not up.
        const bool due = running && now_ms >= since && now_ms - since >= delay_ms;
        d.want_up = f.ap_up || due;
        d.why = d.want_up ? Why::StationDown : Why::Waiting;
        if (!d.want_up && running) {
            d.next_eval_ms = add_sat(since, delay_ms);
            const uint64_t left = d.next_eval_ms - now_ms;  // > 0: !due
            d.up_in_ms = left > uint64_t{INT64_MAX} ? INT64_MAX : static_cast<int64_t>(left);
        }
    }

    if (d.want_up && !f.ap_up) {
        if (now_ms >= bringup_retry_at_ms_) d.action = Action::BringUp;
        else d.next_eval_ms = std::min(d.next_eval_ms, bringup_retry_at_ms_);
    } else if (!d.want_up && f.ap_up) {
        d.action = Action::TakeDown;
    }

    // Station retries scan every channel and take the AP off its channel, so they
    // pause while a phone is on it, except inside the join window an explicit
    // join or restore() opened (SPEC.md §9.5).
    const bool pausable = s.enabled && f.provisioned && f.ap_up && f.clients > 0 && !f.sta_has_ip;
    if (pausable) {
        if (join_window_open(now_ms)) d.next_eval_ms = std::min(d.next_eval_ms, join_until_ms_);  // pause starts then
        else d.pause_sta = true;
    }
    if (d.pause_sta && kPausedProbeMs > 0) {
        const uint64_t due_at = add_sat(last_probe_ms_, kPausedProbeMs);
        d.probe_now = now_ms >= due_at;
        d.next_eval_ms = std::min(d.next_eval_ms, d.probe_now ? add_sat(now_ms, kPausedProbeMs) : due_at);
    }
    return d;
}

}  // namespace dfu32::ap
