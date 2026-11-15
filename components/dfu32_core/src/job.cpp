// SPDX-License-Identifier: MIT
#include "dfu32_core/job.hpp"

#include <initializer_list>

#include "dfu32_core/json.hpp"
#include "dfu32_core/target_config.hpp"

namespace dfu32::job {

const char* to_string(State s) noexcept {
    switch (s) {
        case State::Idle: return "IDLE";
        case State::Fetching: return "FETCHING";
        case State::VerifyingStaged: return "VERIFYING_STAGED";
        case State::Precheck: return "PRECHECK";
        case State::Staged: return "STAGED";
        case State::AwaitingTarget: return "AWAITING_TARGET";
        case State::RadioHandover: return "RADIO_HANDOVER";
        case State::DfuRunning: return "DFU_RUNNING";
        case State::Restoring: return "RESTORING";
        case State::Succeeded: return "SUCCEEDED";
        case State::Failed: return "FAILED";
    }
    return "?";
}

const char* to_string(Kind k) noexcept { return k == Kind::Flash ? "flash" : "stage"; }
const char* to_string(Source s) noexcept { return s == Source::Staged ? "staged" : "manifest"; }

bool is_terminal(State s) noexcept { return s == State::Staged || s == State::Succeeded || s == State::Failed; }

bool is_active(State s) noexcept { return s != State::Idle && !is_terminal(s); }

bool can_transition(State from, State to, Kind kind) noexcept {
    const bool flash = kind == Kind::Flash;
    switch (from) {
        case State::Idle: return false;  // only Machine::start leaves Idle
        case State::Fetching: return to == State::VerifyingStaged || to == State::Failed;
        case State::VerifyingStaged: return to == State::Precheck || to == State::Failed;
        case State::Precheck:
            return (to == State::Staged && !flash) || (to == State::AwaitingTarget && flash) || to == State::Failed;
        case State::AwaitingTarget: return to == State::RadioHandover || to == State::Failed;
        case State::RadioHandover: return to == State::DfuRunning || to == State::Restoring;
        case State::DfuRunning: return to == State::Restoring;
        case State::Restoring: return to == State::Succeeded || to == State::Failed;
        case State::Staged:
        case State::Succeeded:
        case State::Failed: return to == State::Idle;
    }
    return false;
}

bool Machine::start(Kind kind, Source source) {
    if (is_active(state_)) return false;
    kind_ = kind;
    source_ = kind == Kind::Stage ? Source::Manifest : source;
    handover_ = false;
    state_ = source_ == Source::Manifest ? State::Fetching : State::VerifyingStaged;
    return true;
}

bool Machine::advance(State to) {
    if (!can_transition(state_, to, kind_)) return false;
    if (to == State::RadioHandover) handover_ = true;
    state_ = to;
    return true;
}

void Machine::reset() {
    if (is_terminal(state_)) state_ = State::Idle;
}

namespace {

std::string as_object(std::string_view body, json::Doc& doc, json::Node& root) {
    // An empty body (curl -X POST without -d) means "all defaults".
    bool blank = true;
    for (const char c : body) blank = blank && (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (blank) return {};
    std::string err;
    if (!doc.parse(body, &err)) return err;
    root = doc.root();
    if (!root.is_object()) return "body must be a JSON object";
    return {};
}

std::string unknown_key(const json::Node& root, std::initializer_list<std::string_view> known) {
    for (const json::Node& item : root.items()) {
        bool ok = false;
        for (const auto k : known) ok = ok || item.key() == k;
        if (!ok) return "unknown field '" + std::string(item.key()) + "'";
    }
    return {};
}

}  // namespace

std::string parse_flash_request(std::string_view body, const FlashRequest& defaults, FlashRequest& out) {
    json::Doc doc;
    json::Node root;
    if (auto e = as_object(body, doc, root); !e.empty()) return e;
    FlashRequest r = defaults;
    if (!root.missing()) {
        if (auto e = unknown_key(root, {"source", "arm_timeout_s", "wifi_off"}); !e.empty()) return e;
        if (const json::Node f = root.get("source"); !f.missing()) {
            const std::string s = f.str();
            if (s == "manifest") {
                r.source = Source::Manifest;
            } else if (s == "staged") {
                r.source = Source::Staged;
            } else {
                return "source must be \"manifest\" or \"staged\"";
            }
        }
        if (const json::Node f = root.get("arm_timeout_s"); !f.missing()) {
            int64_t v = 0;
            if (!f.integer(v) || v < kMinArmTimeoutS || v > kMaxArmTimeoutS) return "arm_timeout_s must be 10-3600";
            r.arm_timeout_s = static_cast<uint32_t>(v);
        }
        if (const json::Node f = root.get("wifi_off"); !f.missing()) {
            if (!f.is_bool()) return "wifi_off must be true or false";
            r.wifi_off = f.boolean();
        }
    }
    out = r;
    return {};
}

std::string parse_stage_request(std::string_view body, StageRequest& out) {
    json::Doc doc;
    json::Node root;
    if (auto e = as_object(body, doc, root); !e.empty()) return e;
    StageRequest r;
    if (!root.missing()) {
        if (auto e = unknown_key(root, {"force", "url"}); !e.empty()) return e;
        if (const json::Node f = root.get("force"); !f.missing()) {
            if (!f.is_bool()) return "force must be true or false";
            r.force = f.boolean();
        }
        if (const json::Node f = root.get("url"); !f.missing()) {
            if (!f.is_string()) return "url must be a string";
            r.manifest_url = f.str();
            if (auto e = validate_manifest_url(r.manifest_url); !e.empty()) return e;
        }
    }
    out = std::move(r);
    return {};
}

uint32_t run_deadline_s(uint32_t attempts, uint32_t bin_size, uint32_t cap_s) noexcept {
    const uint64_t per_attempt = 60 + (uint64_t{bin_size} + 1999) / 2000;
    const uint64_t d = 120 + uint64_t{attempts} * per_attempt;
    return static_cast<uint32_t>(d < cap_s ? d : cap_s);
}

RestoreStep restore_step(bool expect_connect, bool sta_ip, bool fault, bool radio_alive, uint8_t attempt) noexcept {
    // SPEC.md §7.3 layer 4: a network that was down before the transfer is
    // never a reason to reboot.
    if (!expect_connect) return RestoreStep::Done;
    // Fault injection simulates a dead radio: it ignores the IP and the
    // driver's activity so the reboot path can be tested on the bench.
    if (fault) return attempt < 2 ? RestoreStep::Retry : RestoreStep::Reboot;
    if (sta_ip) return RestoreStep::Done;
    if (attempt < 2) return RestoreStep::Retry;
    // Out of retries: reboot only when the driver shows no sign of life. A
    // working radio means the home network is gone, which a reboot can't fix,
    // and the phones on DFU32's access point would lose it (§9.5).
    return radio_alive ? RestoreStep::Done : RestoreStep::Reboot;
}

OtaVerdict ota_verdict(bool sta_ip, bool ap_started, bool provisioned, uint8_t last_reason) noexcept {
    if (sta_ip) return OtaVerdict::Confirm;
    // §9.5: the access point only starts if the WiFi driver works, which is
    // all this test is meant to prove about the new image.
    if (ap_started) return OtaVerdict::ConfirmAccessPoint;
    if (!provisioned) return OtaVerdict::ConfirmNoNetwork;
    if (last_reason != 0) return OtaVerdict::ConfirmRadioWorks;
    return OtaVerdict::Rollback;
}

void write_json(json::ObjectWriter& w, const Snapshot& s, uint32_t now_uptime_s) {
    w.add("id", int64_t{s.id}).add("state", to_string(s.state));
    if (s.id == 0) return;
    w.add("kind", to_string(s.kind))
        .add("source", to_string(s.source))
        .add("target", s.target)
        .add("phase", s.phase)
        .add("version", s.version)
        .add("attempt", int64_t{s.attempt})
        .add("max_attempts", int64_t{s.max_attempts})
        .add("bytes_done", int64_t{s.bytes_done})
        .add("bytes_total", int64_t{s.bytes_total})
        .add("rate_bps", int64_t{s.rate_bps})
        .add("started_uptime_s", int64_t{s.started_uptime_s})
        .add_bool("wifi_off", s.wifi_off)
        .add_bool("target_erased", s.target_erased);
    if (s.state == State::AwaitingTarget) {
        const uint32_t left = s.arm_deadline_uptime_s > now_uptime_s ? s.arm_deadline_uptime_s - now_uptime_s : 0;
        w.add("arm_remaining_s", int64_t{left});
    }
    if (s.kind == Kind::Stage) w.add_bool("reused", s.reused);
    if (!s.bootloader.empty()) w.add("bootloader", s.bootloader);
    if (!s.error.empty()) w.add("error", s.error);
    if (s.run_seq != 0) w.add("run", int64_t{s.run_seq});
}

}  // namespace dfu32::job
