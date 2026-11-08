// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "indicator/logic.hpp"

using namespace dfu32::indicator;
namespace job = dfu32::job;

namespace {

job::Snapshot snap(uint32_t id, job::State st, job::Kind kind = job::Kind::Flash, bool erased = false) {
    job::Snapshot s;
    s.id = id;
    s.state = st;
    s.kind = kind;
    s.target_erased = erased;
    return s;
}

Inputs idle() {
    Inputs in;
    in.provisioned = true;
    in.wifi_connected = true;
    return in;
}

}  // namespace

TEST_CASE("Indicator: idle, offline and Improv", "[indicator]") {
    Inputs in = idle();
    CHECK(decide(in).pattern == Pattern::Idle);
    CHECK(decide(in).red == RedLed::Off);
    in.wifi_connected = false;
    CHECK(decide(in).pattern == Pattern::Offline);
    in.wifi_stopped = true;  // stopped for a transfer: not "offline"
    CHECK(decide(in).pattern == Pattern::Idle);
    Inputs unprov;
    CHECK(decide(unprov).pattern == Pattern::Idle);
    unprov.improv_active = true;
    CHECK(decide(unprov).pattern == Pattern::Improv);
}

TEST_CASE("Indicator: job phases", "[indicator]") {
    Inputs in = idle();
    for (auto s : {job::State::Fetching, job::State::VerifyingStaged, job::State::Precheck}) {
        in.job_state = s;
        CHECK(decide(in).pattern == Pattern::Fetching);
    }
    in.job_state = job::State::AwaitingTarget;
    CHECK(decide(in).pattern == Pattern::Awaiting);
    in.job_state = job::State::DfuRunning;
    in.bytes_total = 1000;
    in.bytes_done = 0;
    Output o = decide(in);
    CHECK(o.pattern == Pattern::Transfer);
    CHECK(o.brightness == 40);
    CHECK(o.red == RedLed::Blink);
    in.bytes_done = 500;
    CHECK(decide(in).brightness == 147);
    in.bytes_done = 1000;
    CHECK(decide(in).brightness == 255);
    in.bytes_total = 0;  // not known yet
    CHECK(decide(in).brightness == 40);
}

TEST_CASE("Indicator: outcomes and their priorities", "[indicator]") {
    Inputs in = idle();
    in.success_showing = true;
    CHECK(decide(in).pattern == Pattern::Success);
    in.failed = true;
    CHECK(decide(in).pattern == Pattern::Failed);
    in.erased_pending = true;
    Output o = decide(in);
    CHECK(o.pattern == Pattern::Erased);
    CHECK(o.red == RedLed::On);
    in.improv_active = true;
    CHECK(decide(in).pattern == Pattern::Improv);
    // A running job (e.g. the re-flash that fixes the erased target) shows its
    // progress over stale outcomes.
    in.job_state = job::State::AwaitingTarget;
    o = decide(in);
    CHECK(o.pattern == Pattern::Awaiting);
    CHECK(o.red == RedLed::On);
    in.identify = true;
    CHECK(decide(in).pattern == Pattern::Identify);
    in.hold_level = 2;
    CHECK(decide(in).pattern == Pattern::Hold2);
    in.hold_level = 5;
    CHECK(decide(in).pattern == Pattern::Hold5);
    in.hold_level = 15;
    CHECK(decide(in).pattern == Pattern::Hold15);
}

TEST_CASE("Indicator: tracker", "[indicator]") {
    Tracker t;
    t.update(job::Snapshot{}, 0);  // no job since boot
    CHECK_FALSE(t.failed());
    t.update(snap(1, job::State::DfuRunning, job::Kind::Flash, true), 10);
    CHECK_FALSE(t.erased_pending());  // only a finished job counts
    t.update(snap(1, job::State::Failed, job::Kind::Flash, true), 11);
    CHECK(t.failed());
    CHECK(t.erased_pending());
    // A later stage-only job clears "failed" but not "erased".
    t.update(snap(2, job::State::Fetching, job::Kind::Stage), 20);
    CHECK_FALSE(t.failed());
    CHECK(t.erased_pending());
    t.update(snap(2, job::State::Failed, job::Kind::Stage), 21);
    CHECK(t.failed());
    CHECK(t.erased_pending());
    // A successful flash fixes it and shows green for 10 s.
    t.update(snap(3, job::State::Succeeded), 30);
    CHECK_FALSE(t.failed());
    CHECK_FALSE(t.erased_pending());
    CHECK(t.success_showing(39));
    CHECK_FALSE(t.success_showing(40));
    t.update(snap(3, job::State::Succeeded), 45);  // same job seen again: no restart
    CHECK_FALSE(t.success_showing(45));
    // A failure that never reached START doesn't mark the target erased.
    t.update(snap(4, job::State::Failed, job::Kind::Flash, false), 50);
    CHECK(t.failed());
    CHECK_FALSE(t.erased_pending());
}

TEST_CASE("Indicator: button holds", "[indicator]") {
    CHECK(classify_hold(0) == ButtonAction::None);
    CHECK(classify_hold(1999) == ButtonAction::None);
    CHECK(classify_hold(2000) == ButtonAction::ImprovWindow);
    CHECK(classify_hold(4999) == ButtonAction::ImprovWindow);
    CHECK(classify_hold(5000) == ButtonAction::Unclaim);
    CHECK(classify_hold(14999) == ButtonAction::Unclaim);
    CHECK(classify_hold(15000) == ButtonAction::FactoryReset);
    CHECK(hold_level(1500) == 0);
    CHECK(hold_level(2500) == 2);
    CHECK(hold_level(6000) == 5);
    CHECK(hold_level(20000) == 15);
}
