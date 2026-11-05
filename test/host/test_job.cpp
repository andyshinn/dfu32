// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "dfu32_core/job.hpp"
#include "dfu32_core/json.hpp"

using namespace dfu32::job;
namespace json = dfu32::json;

TEST_CASE("Job: flash from the manifest walks the SPEC §7 happy path", "[job]") {
    Machine m;
    CHECK(m.state() == State::Idle);
    REQUIRE(m.start(Kind::Flash, Source::Manifest));
    CHECK(m.state() == State::Fetching);
    CHECK_FALSE(m.start(Kind::Stage, Source::Manifest));  // one job at a time (409 Busy)
    for (State s : {State::VerifyingStaged, State::Precheck, State::AwaitingTarget, State::RadioHandover,
                    State::DfuRunning, State::Restoring, State::Succeeded}) {
        REQUIRE(m.advance(s));
    }
    CHECK(m.handover_started());
    CHECK(is_terminal(m.state()));
    CHECK_FALSE(is_active(m.state()));
    m.reset();
    CHECK(m.state() == State::Idle);
}

TEST_CASE("Job: flash from staging skips the fetch", "[job]") {
    Machine m;
    REQUIRE(m.start(Kind::Flash, Source::Staged));
    CHECK(m.state() == State::VerifyingStaged);
    CHECK_FALSE(m.advance(State::Fetching));
    CHECK(m.advance(State::Precheck));
    CHECK_FALSE(m.advance(State::Staged));  // that edge is for stage-only jobs
    CHECK(m.advance(State::AwaitingTarget));
}

TEST_CASE("Job: stage-only jobs end in STAGED and always fetch", "[job]") {
    Machine m;
    REQUIRE(m.start(Kind::Stage, Source::Staged));  // source is ignored for stage
    CHECK(m.state() == State::Fetching);
    CHECK(m.source() == Source::Manifest);
    REQUIRE(m.advance(State::VerifyingStaged));
    REQUIRE(m.advance(State::Precheck));
    CHECK_FALSE(m.advance(State::AwaitingTarget));
    REQUIRE(m.advance(State::Staged));
    CHECK(m.start(Kind::Flash, Source::Staged));  // a finished job doesn't block the next one
}

TEST_CASE("Job: failures before the handover go straight to FAILED", "[job]") {
    for (State at : {State::Fetching, State::VerifyingStaged, State::Precheck, State::AwaitingTarget}) {
        Machine m;
        REQUIRE(m.start(Kind::Flash, Source::Manifest));
        // Walk to `at`.
        const State path[] = {State::Fetching, State::VerifyingStaged, State::Precheck, State::AwaitingTarget};
        for (State s : path) {
            if (s == State::Fetching) continue;
            if (m.state() == at) break;
            REQUIRE(m.advance(s));
        }
        REQUIRE(m.state() == at);
        CHECK(m.advance(State::Failed));
        CHECK_FALSE(m.handover_started());
    }
}

TEST_CASE("Job: after the handover, every exit goes through RESTORING", "[job]") {
    Machine m;
    REQUIRE(m.start(Kind::Flash, Source::Staged));
    for (State s : {State::Precheck, State::AwaitingTarget, State::RadioHandover}) REQUIRE(m.advance(s));
    CHECK_FALSE(m.advance(State::Failed));  // WiFi might be down: must restore first
    CHECK_FALSE(m.advance(State::Succeeded));
    CHECK(m.advance(State::Restoring));  // abort during the handover
    CHECK(m.advance(State::Failed));

    Machine r;
    REQUIRE(r.start(Kind::Flash, Source::Staged));
    for (State s : {State::Precheck, State::AwaitingTarget, State::RadioHandover, State::DfuRunning}) {
        REQUIRE(r.advance(s));
    }
    CHECK_FALSE(r.advance(State::Failed));
    CHECK(r.advance(State::Restoring));
    CHECK_FALSE(r.advance(State::DfuRunning));
}

TEST_CASE("Job: reset only clears finished jobs", "[job]") {
    Machine m;
    REQUIRE(m.start(Kind::Stage, Source::Manifest));
    m.reset();
    CHECK(m.state() == State::Fetching);
    CHECK_FALSE(m.advance(State::Idle));
}

TEST_CASE("Job: flash request parsing", "[job]") {
    FlashRequest def;
    def.arm_timeout_s = 300;
    def.wifi_off = false;
    FlashRequest r;
    CHECK(parse_flash_request("", def, r).empty());
    CHECK(r.source == Source::Manifest);
    CHECK(r.arm_timeout_s == 300);
    CHECK_FALSE(r.wifi_off);
    CHECK(parse_flash_request("  \n", def, r).empty());

    CHECK(parse_flash_request(R"({"source":"staged","arm_timeout_s":60,"wifi_off":true})", def, r).empty());
    CHECK(r.source == Source::Staged);
    CHECK(r.arm_timeout_s == 60);
    CHECK(r.wifi_off);

    CHECK(parse_flash_request(R"({"source":"usb"})", def, r) == "source must be \"manifest\" or \"staged\"");
    CHECK(parse_flash_request(R"({"arm_timeout_s":5})", def, r) == "arm_timeout_s must be 10-3600");
    CHECK(parse_flash_request(R"({"arm_timeout_s":3601})", def, r) == "arm_timeout_s must be 10-3600");
    CHECK(parse_flash_request(R"({"wifi_off":1})", def, r) == "wifi_off must be true or false");
    CHECK(parse_flash_request(R"({"wifioff":true})", def, r) == "unknown field 'wifioff'");
    CHECK_FALSE(parse_flash_request("[]", def, r).empty());
    CHECK_FALSE(parse_flash_request("{", def, r).empty());
}

TEST_CASE("Job: stage request parsing", "[job]") {
    StageRequest r;
    CHECK(parse_stage_request("", r).empty());
    CHECK_FALSE(r.force);
    CHECK(r.manifest_url.empty());
    CHECK(parse_stage_request(R"({"force":true,"url":"http://fw.lan/m.json"})", r).empty());
    CHECK(r.force);
    CHECK(r.manifest_url == "http://fw.lan/m.json");
    CHECK_FALSE(parse_stage_request(R"({"url":"https://fw.lan/m.json"})", r).empty());
    CHECK_FALSE(parse_stage_request(R"({"force":"yes"})", r).empty());
    CHECK_FALSE(parse_stage_request(R"({"other":1})", r).empty());
}

TEST_CASE("Job: run deadline (SPEC §7.3)", "[job]") {
    // 413,680 B (the bench image), 3 attempts: 120 + 3 x (60 + 207) = 921 s.
    CHECK(run_deadline_s(3, 413680, 1200) == 921);
    // 798,720 B, 3 attempts: 120 + 3 x (60 + 400) = 1500 -> capped.
    CHECK(run_deadline_s(3, 798720, 1200) == 1200);
    CHECK(run_deadline_s(1, 0, 1200) == 180);
    CHECK(run_deadline_s(1, 1, 1200) == 181);  // rounds up
}

TEST_CASE("Job: snapshot JSON", "[job]") {
    {
        json::ObjectWriter w;
        write_json(w, Snapshot{}, 0);
        CHECK(w.str() == R"({"id":0,"state":"IDLE"})");
    }
    Snapshot s;
    s.id = 3;
    s.state = State::AwaitingTarget;
    s.kind = Kind::Flash;
    s.target = "attic";
    s.phase = "SCAN";
    s.arm_deadline_uptime_s = 700;
    s.max_attempts = 3;
    json::ObjectWriter w;
    write_json(w, s, 100);
    json::Doc d;
    REQUIRE(d.parse(w.str()));
    const auto root = d.root();
    CHECK(root.get("state").str() == "AWAITING_TARGET");
    CHECK(root.get("kind").str() == "flash");
    CHECK(root.get("target").str() == "attic");
    int64_t left = 0;
    REQUIRE(root.get("arm_remaining_s").integer(left));
    CHECK(left == 600);
    CHECK(root.get("error").missing());
    CHECK(root.get("reused").missing());

    s.state = State::Failed;
    s.error = "BOARD_MISMATCH: x";
    s.run_seq = 12;
    json::ObjectWriter w2;
    write_json(w2, s, 100);
    json::Doc d2;
    REQUIRE(d2.parse(w2.str()));
    CHECK(d2.root().get("error").str() == "BOARD_MISMATCH: x");
    CHECK(d2.root().get("arm_remaining_s").missing());
    int64_t run = 0;
    REQUIRE(d2.root().get("run").integer(run));
    CHECK(run == 12);
}
