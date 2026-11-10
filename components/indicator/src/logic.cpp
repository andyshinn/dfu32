// SPDX-License-Identifier: MIT
#include "indicator/logic.hpp"

namespace dfu32::indicator {

void Tracker::update(const job::Snapshot& s, uint32_t now_s) {
    if (s.id == 0) return;
    if (s.id != seen_id_) {
        // A new job: the last outcome no longer applies (an erased target
        // stays erased until a flash succeeds).
        seen_id_ = s.id;
        seen_terminal_ = false;
        failed_ = false;
        success_until_ = 0;
    }
    if (seen_terminal_ || !job::is_terminal(s.state)) return;
    seen_terminal_ = true;
    if (s.state == job::State::Failed) {
        failed_ = true;
        if (s.kind == job::Kind::Flash && s.target_erased) erased_ = true;
    } else if (s.state == job::State::Succeeded) {
        erased_ = false;
        success_until_ = now_s + kSuccessShowS;
    }
}

Output decide(const Inputs& in) {
    Output o;
    const bool active = job::is_active(in.job_state);
    using S = job::State;

    // The red LED: blinking during the transfer, solid while the target is
    // known to be erased (SPEC.md §13.4).
    if (in.job_state == S::DfuRunning) {
        o.red = RedLed::Blink;
    } else if (in.erased_pending) {
        o.red = RedLed::On;
    }

    if (in.hold_level >= 15) {
        o.pattern = Pattern::Hold15;
    } else if (in.hold_level >= 5) {
        o.pattern = Pattern::Hold5;
    } else if (in.hold_level >= 2) {
        o.pattern = Pattern::Hold2;
    } else if (in.identify) {
        o.pattern = Pattern::Identify;
    } else if (active) {
        switch (in.job_state) {
            case S::AwaitingTarget: o.pattern = Pattern::Awaiting; break;
            case S::RadioHandover:
            case S::DfuRunning:
            case S::Restoring: {
                o.pattern = Pattern::Transfer;
                const uint32_t pct =
                    in.bytes_total ? static_cast<uint32_t>(uint64_t{in.bytes_done} * 100 / in.bytes_total) : 0;
                o.brightness = static_cast<uint8_t>(40 + (pct > 100 ? 100 : pct) * 215 / 100);
                break;
            }
            default: o.pattern = Pattern::Fetching; break;
        }
    } else if (in.improv_active) {
        o.pattern = Pattern::Improv;
    } else if (in.erased_pending) {
        o.pattern = Pattern::Erased;
    } else if (in.failed) {
        o.pattern = Pattern::Failed;
    } else if (in.success_showing) {
        o.pattern = Pattern::Success;
    } else if (in.provisioned && !in.wifi_connected && !in.wifi_stopped) {
        o.pattern = Pattern::Offline;
    } else {
        o.pattern = Pattern::Idle;
    }
    if (o.pattern != Pattern::Transfer) o.brightness = 255;
    return o;
}

ButtonAction classify_hold(uint32_t held_ms) noexcept {
    if (held_ms >= 15000) return ButtonAction::FactoryReset;
    if (held_ms >= 5000) return ButtonAction::ClearToken;
    if (held_ms >= 2000) return ButtonAction::ImprovWindow;
    return ButtonAction::None;
}

uint8_t hold_level(uint32_t held_ms) noexcept {
    if (held_ms >= 15000) return 15;
    if (held_ms >= 5000) return 5;
    if (held_ms >= 2000) return 2;
    return 0;
}

}  // namespace dfu32::indicator
