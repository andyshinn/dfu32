// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

#include "dfu32_core/ap.hpp"

using namespace dfu32::ap;

namespace {

Facts facts(bool provisioned, bool ip, bool ap_up, uint8_t clients, bool stopped = false) {
    Facts f;
    f.stopped = stopped;
    f.provisioned = provisioned;
    f.sta_has_ip = ip;
    f.ap_up = ap_up;
    f.clients = clients;
    return f;
}

Settings on(uint32_t delay_s = 120) {
    Settings s;
    s.enabled = true;
    s.delay_s = delay_s;
    return s;
}

Command parse(std::initializer_list<const char*> args) {
    std::vector<const char*> argv{"ap"};
    argv.insert(argv.end(), args.begin(), args.end());
    return parse_command(static_cast<int>(argv.size()), argv.data());
}

// Plays net's part: evaluates whenever told and at every next_eval_ms, and makes
// the driver obey each action (a TakeDown drops the phones with the AP).
struct Sim {
    Controller c;
    Settings s = on();
    Facts f = facts(true, false, false, 0);
    uint64_t now = 0;
    Decision d;
    std::vector<uint64_t> ups, downs;

    void eval() {
        for (int i = 0; i < 3; ++i) {
            d = c.step(s, f, now);
            if (d.action == Action::BringUp) {
                f.ap_up = true;
                ups.push_back(now);
            } else if (d.action == Action::TakeDown) {
                f.ap_up = false;
                f.clients = 0;
                downs.push_back(now);
            } else {
                return;
            }
        }
        FAIL("the decision did not settle");
    }
    void run_to(uint64_t t) {
        eval();
        while (d.next_eval_ms != kNever && d.next_eval_ms <= t) {
            REQUIRE(d.next_eval_ms > now);
            now = d.next_eval_ms;
            eval();
        }
        now = t;
        eval();
    }
    void lose_ip() {
        f.sta_has_ip = false;
        c.ip_lost(now);
        eval();
    }
    void get_ip() {
        f.sta_has_ip = true;
        c.ip_got();
        eval();
    }
    void clients(uint8_t n) {
        f.clients = f.ap_up ? n : 0;
        eval();
    }
};

}  // namespace

// ---- validators ----------------------------------------------------------------------

TEST_CASE("AP: password validation", "[ap]") {
    CHECK(validate_ap_password("").empty());  // open
    CHECK_FALSE(validate_ap_password("1234567").empty());
    CHECK(validate_ap_password("12345678").empty());
    CHECK(validate_ap_password(std::string(63, 'x')).empty());
    CHECK_FALSE(validate_ap_password(std::string(64, 'x')).empty());
    CHECK_FALSE(validate_ap_password(std::string(64, 'a')).empty());  // 64 hex: no raw PSK for the AP
    CHECK_FALSE(validate_ap_password("abcdefg\x1F").empty());
    CHECK_FALSE(validate_ap_password("abcdefg\x7F").empty());
    CHECK_FALSE(validate_ap_password("abcd\tefgh").empty());
    CHECK_FALSE(validate_ap_password("abcdefg\xC3\xA9").empty());
    CHECK(validate_ap_password("a b \"c\" \\d").empty());
    CHECK(validate_ap_password("1234567") == "ap_password must be 8-63 printable ASCII characters");
}

TEST_CASE("AP: delay validation", "[ap]") {
    CHECK_FALSE(validate_ap_delay(29).empty());
    CHECK_FALSE(validate_ap_delay(1801).empty());
    CHECK_FALSE(validate_ap_delay(0).empty());
    CHECK_FALSE(validate_ap_delay(UINT64_MAX).empty());
    CHECK(validate_ap_delay(30).empty());
    CHECK(validate_ap_delay(120).empty());
    CHECK(validate_ap_delay(1800).empty());
    CHECK(validate_ap_delay(29) == "ap_delay_s must be 30-1800");
}

TEST_CASE("AP: station credentials validation", "[ap]") {
    CHECK_FALSE(validate_wifi_credentials("", "").empty());
    CHECK_FALSE(validate_wifi_credentials(std::string(33, 's'), "").empty());
    CHECK(validate_wifi_credentials(std::string(32, 's'), "").empty());
    CHECK(validate_wifi_credentials("home", "").empty());
    CHECK(validate_wifi_credentials(std::string("a\0b\xFF", 4), "").empty());  // any bytes
    CHECK_FALSE(validate_wifi_credentials("home", "1234567").empty());
    CHECK(validate_wifi_credentials("home", "12345678").empty());
    CHECK(validate_wifi_credentials("home", std::string(63, 'p')).empty());
    CHECK(validate_wifi_credentials("home", std::string(32, 'a') + std::string(32, 'F')).empty());
    CHECK_FALSE(validate_wifi_credentials("home", std::string(63, 'a') + "g").empty());
    CHECK_FALSE(validate_wifi_credentials("home", std::string(65, 'a')).empty());
    CHECK_FALSE(validate_wifi_credentials("home", "abcdefg\n").empty());
}

TEST_CASE("AP: load sanitising never opens the AP", "[ap]") {
    bool enabled = true;
    std::string pw = "short";
    uint32_t delay = 300;
    LoadFixups fx = sanitize_loaded(enabled, pw, delay);
    CHECK(fx.password_invalid);
    CHECK_FALSE(fx.delay_reset);
    CHECK_FALSE(enabled);
    CHECK(pw.empty());
    CHECK(delay == 300);

    for (uint32_t bad : {0u, 5000u}) {
        enabled = true;
        pw = "longenough";
        delay = bad;
        fx = sanitize_loaded(enabled, pw, delay);
        CHECK(fx.delay_reset);
        CHECK_FALSE(fx.password_invalid);
        CHECK(delay == kDefaultDelayS);
        CHECK(enabled);
        CHECK(pw == "longenough");
    }

    enabled = true;
    pw = "";
    delay = 1800;
    fx = sanitize_loaded(enabled, pw, delay);
    CHECK_FALSE(fx.password_invalid);
    CHECK_FALSE(fx.delay_reset);
    CHECK(enabled);
    CHECK(pw.empty());
    CHECK(delay == 1800);
}

TEST_CASE("AP: channel", "[ap]") {
    CHECK(ap_channel(6, 1, 11) == 6);
    CHECK(ap_channel(12, 1, 11) == 1);
    CHECK(ap_channel(13, 1, 13) == 13);
    CHECK(ap_channel(36, 1, 13) == 1);  // a 5 GHz station channel
    CHECK(ap_channel(0, 1, 11) == 1);   // never associated
    CHECK(ap_channel(5, 0, 0) == 1);
}

TEST_CASE("AP: open-AP warning", "[ap]") {
    CHECK(ap_warning(true, true, true, true) == nullptr);    // a token is set
    CHECK(ap_warning(true, false, false, true) == nullptr);  // WPA2
    CHECK(ap_warning(false, true, false, true) == nullptr);  // AP mode off
    const char* active = ap_warning(true, true, false, true);
    const char* standby = ap_warning(true, true, false, false);
    REQUIRE(active != nullptr);
    REQUIRE(standby != nullptr);
    CHECK(std::strcmp(active, standby) != 0);
    CHECK(std::string(active).find("is up and open") != std::string::npos);
    CHECK(std::string(standby).find("whenever the access point comes up") != std::string::npos);
}

TEST_CASE("AP: station failure reasons", "[ap]") {
    for (uint8_t r : {2, 14, 15, 202, 204}) CHECK(std::string(describe_sta_reason(r)) == "wrong password");
    for (uint8_t r : {201, 210, 211, 212}) CHECK(std::string(describe_sta_reason(r)) == "network not found");
    CHECK(describe_sta_reason(0) == nullptr);
    CHECK(std::string(describe_sta_reason(99)) == "could not connect");
}

TEST_CASE("AP: console grammar", "[ap]") {
    using K = Command::Kind;
    CHECK(parse({}).kind == K::Status);
    CHECK(parse_command(1, nullptr).kind == K::Status);
    CHECK(parse({"on"}).kind == K::On);
    CHECK(parse({"off"}).kind == K::Off);
    CHECK(parse({"password", "clear"}).kind == K::ClearPassword);
    Command c = parse({"password", "a b c d e"});
    CHECK(c.kind == K::SetPassword);
    CHECK(c.password == "a b c d e");
    c = parse({"delay", "1800"});
    CHECK(c.kind == K::Delay);
    CHECK(c.delay_s == 1800);
    CHECK(parse({"delay", "0x1e"}).delay_s == 30);  // parse_u32 rules

    c = parse({"password", "short"});
    CHECK(c.kind == K::Invalid);
    CHECK(c.error == "ap_password must be 8-63 printable ASCII characters");
    c = parse({"delay", "29"});
    CHECK(c.kind == K::Invalid);
    CHECK(c.error == "ap_delay_s must be 30-1800");
    c = parse({"delay", "x"});
    CHECK(c.kind == K::Invalid);
    CHECK(c.error == "not a number");
    CHECK(parse({"delay", "-5"}).kind == K::Invalid);

    CHECK(parse({"password"}).kind == K::Usage);
    CHECK(parse({"delay"}).kind == K::Usage);
    CHECK(parse({"off", "extra"}).kind == K::Usage);
    CHECK(parse({"password", "longenough", "x"}).kind == K::Usage);
    CHECK(parse({"bogus"}).kind == K::Usage);
}

// ---- the rule table (SPEC.md §9.5) -----------------------------------------------------

TEST_CASE("AP: an explicit off wins, and a handover freezes the AP", "[ap]") {
    Controller c;
    c.boot(0);
    Settings off;
    Decision d = c.step(off, facts(true, false, true, 2), 1000);
    CHECK_FALSE(d.want_up);
    CHECK(d.action == Action::TakeDown);
    CHECK_FALSE(d.pause_sta);
    CHECK(d.why == Why::Disabled);

    for (bool up : {false, true}) {
        d = c.step(on(), facts(true, false, up, up ? 1 : 0, true), 500000);
        CHECK(d.want_up == up);
        CHECK(d.action == Action::None);
        CHECK_FALSE(d.pause_sta);
        CHECK(d.next_eval_ms == kNever);
        CHECK(d.why == Why::Stopped);
        d = c.step(off, facts(true, false, up, 0, true), 500000);
        CHECK(d.action == Action::None);
    }
}

TEST_CASE("AP: no network stored brings the AP up at once", "[ap]") {
    Controller c;
    c.boot(0);
    const Decision d = c.step(on(), facts(false, false, false, 0), 0);
    CHECK(d.want_up);
    CHECK(d.action == Action::BringUp);
    CHECK(d.why == Why::NoNetworkStored);
}

TEST_CASE("AP: the start delay boundary", "[ap]") {
    for (uint32_t delay : {30u, 120u, 1800u}) {
        INFO(delay);
        const uint64_t ms = uint64_t{delay} * 1000;
        Controller c;
        c.boot(0);
        Decision d = c.step(on(delay), facts(true, false, false, 0), ms - 1);
        CHECK_FALSE(d.want_up);
        CHECK(d.action == Action::None);
        CHECK(d.why == Why::Waiting);
        CHECK(d.next_eval_ms == ms);
        CHECK(d.up_in_ms == 1);
        d = c.step(on(delay), facts(true, false, false, 0), ms);
        CHECK(d.want_up);
        CHECK(d.action == Action::BringUp);
        CHECK(d.why == Why::StationDown);
        CHECK(d.up_in_ms == -1);
    }
}

TEST_CASE("AP: station up keeps the AP down", "[ap]") {
    Controller c;
    c.boot(0);
    c.ip_got();
    CHECK(c.sta_down_since_ms() == kNever);
    const Decision d = c.step(on(), facts(true, true, false, 0), 900000);
    CHECK_FALSE(d.want_up);
    CHECK(d.action == Action::None);
    CHECK(d.next_eval_ms == kNever);
    CHECK(d.why == Why::StationUp);
}

TEST_CASE("AP: 30 s grace after the station gets an IP", "[ap]") {
    Controller c;
    Decision d = c.step(on(), facts(true, true, true, 0), 0);
    CHECK(d.want_up);
    CHECK(d.why == Why::StationUpGrace);
    CHECK(d.next_eval_ms == kDownGraceMs);
    d = c.step(on(), facts(true, true, true, 0), kDownGraceMs - 1);
    CHECK(d.want_up);
    CHECK(d.action == Action::None);
    d = c.step(on(), facts(true, true, true, 0), kDownGraceMs);
    CHECK_FALSE(d.want_up);
    CHECK(d.action == Action::TakeDown);

    // A client restarts the grace.
    Controller g;
    g.step(on(), facts(true, true, true, 0), 0);
    d = g.step(on(), facts(true, true, true, 1), 10000);
    CHECK(d.want_up);
    CHECK(d.why == Why::ClientsPresent);
    CHECK_FALSE(d.pause_sta);  // the station has an IP
    d = g.step(on(), facts(true, true, true, 0), 12000);
    CHECK(d.next_eval_ms == 42000);
    CHECK(g.step(on(), facts(true, true, true, 0), 41999).want_up);
    CHECK(g.step(on(), facts(true, true, true, 0), 42000).action == Action::TakeDown);
}

TEST_CASE("AP: once up without a station IP it stays up", "[ap]") {
    Controller c;
    c.boot(0);
    const Decision d = c.step(on(), facts(true, false, true, 0), 500000);
    CHECK(d.want_up);
    CHECK(d.action == Action::None);
    CHECK(d.next_eval_ms == kNever);
    CHECK(d.why == Why::StationDown);
}

TEST_CASE("AP: station retries pause for clients, except in the join window", "[ap]") {
    Controller c;
    c.boot(0);
    const Facts phone = facts(true, false, true, 1);
    CHECK(c.step(on(), phone, 1000).pause_sta);
    c.explicit_join(1000);
    Decision d = c.step(on(), phone, 1000);
    CHECK_FALSE(d.pause_sta);
    CHECK(d.next_eval_ms == 1000 + kJoinWindowMs);  // the pause starts then
    CHECK_FALSE(c.step(on(), phone, 1000 + kJoinWindowMs - 1).pause_sta);
    CHECK(c.step(on(), phone, 1000 + kJoinWindowMs).pause_sta);
    CHECK_FALSE(c.step(on(), facts(true, true, true, 1), 100000).pause_sta);
    CHECK_FALSE(c.step(on(), facts(true, false, true, 0), 100000).pause_sta);
    CHECK_FALSE(c.step(on(), facts(false, false, true, 1), 100000).pause_sta);  // nothing to retry
    d = c.step(on(), phone, 100000);
    CHECK_FALSE(d.probe_now);  // strict pause: no probing
}

TEST_CASE("AP: a failed bring-up is retried after 10 s", "[ap]") {
    Controller c;
    c.boot(0);
    CHECK(c.step(on(), facts(false, false, false, 0), 0).action == Action::BringUp);
    c.bringup_failed(0);
    Decision d = c.step(on(), facts(false, false, false, 0), 5000);
    CHECK(d.want_up);
    CHECK(d.action == Action::None);
    CHECK(d.next_eval_ms == kBringUpRetryMs);
    CHECK(c.step(on(), facts(false, false, false, 0), kBringUpRetryMs - 1).action == Action::None);
    CHECK(c.step(on(), facts(false, false, false, 0), kBringUpRetryMs).action == Action::BringUp);
}

TEST_CASE("AP: clock skew never brings the AP up early", "[ap]") {
    Controller c;
    c.boot(50000);
    const Decision d = c.step(on(), facts(true, false, false, 0), 10000);
    CHECK_FALSE(d.want_up);
    CHECK(d.action == Action::None);
    CHECK(d.next_eval_ms == 170000);
    CHECK(d.up_in_ms == 160000);
}

TEST_CASE("AP: the station clock follows the facts when an event is missed", "[ap]") {
    SECTION("a missed ip_lost still brings the AP up after the delay") {
        Controller c;
        c.boot(0);
        c.ip_got();
        CHECK(c.step(on(), facts(true, true, false, 0), 10000).why == Why::StationUp);
        Decision d = c.step(on(), facts(true, false, false, 0), 20000);  // no ip_lost()
        CHECK(c.sta_down_since_ms() == 20000);
        CHECK(d.next_eval_ms == 140000);
        CHECK(c.step(on(), facts(true, false, false, 0), 140000).action == Action::BringUp);
    }
    SECTION("restored() after GOT_IP doesn't shorten the next delay") {
        Controller c;
        c.boot(0);
        c.stopped(true);
        c.ip_got();  // GOT_IP raced ahead of restored()
        c.restored(50000);
        CHECK(c.step(on(), facts(true, true, false, 0), 50000).why == Why::StationUp);
        CHECK(c.sta_down_since_ms() == kNever);
        c.ip_lost(400000);
        CHECK_FALSE(c.step(on(), facts(true, false, false, 0), 400000).want_up);
        CHECK(c.step(on(), facts(true, false, false, 0), 520000).action == Action::BringUp);
    }
    SECTION("the grace only runs once a network is stored") {
        Controller c;
        c.boot(0);
        // forget() while still associated: rule 3 holds the AP up, no grace yet.
        CHECK(c.step(on(), facts(false, true, true, 0), 0).why == Why::NoNetworkStored);
        const Decision d = c.step(on(), facts(true, true, true, 0), 25000);
        CHECK(d.want_up);
        CHECK(d.next_eval_ms == 25000 + kDownGraceMs);
    }
}

// ---- scenario traces -----------------------------------------------------------------------

TEST_CASE("AP: scenario: the router dies", "[ap]") {
    Sim sim;
    sim.c.boot(0);
    sim.get_ip();
    sim.run_to(10000);
    sim.lose_ip();
    sim.run_to(129999);
    CHECK(sim.ups.empty());
    sim.run_to(400000);
    REQUIRE(sim.ups.size() == 1);
    CHECK(sim.ups[0] == 130000);  // reached through next_eval, not a poll
}

TEST_CASE("AP: scenario: a phone sets the station up", "[ap]") {
    Sim sim;
    sim.c.boot(0);
    sim.run_to(125000);
    REQUIRE(sim.f.ap_up);
    sim.clients(1);
    CHECK(sim.d.pause_sta);

    sim.now = 130000;
    sim.c.explicit_join(sim.now);  // PUT /api/v1/wifi -> set_credentials
    sim.eval();
    CHECK_FALSE(sim.d.pause_sta);
    sim.run_to(190000);
    CHECK_FALSE(sim.d.pause_sta);  // unpaused for the whole 65 s window
    sim.get_ip();
    CHECK(sim.d.why == Why::ClientsPresent);
    sim.run_to(600000);
    CHECK(sim.f.ap_up);  // stays up while the phone is there
    CHECK_FALSE(sim.d.pause_sta);
    sim.clients(0);
    sim.run_to(629999);
    CHECK(sim.f.ap_up);
    sim.run_to(700000);
    REQUIRE(sim.downs.size() == 1);
    CHECK(sim.downs[0] == 630000);
}

TEST_CASE("AP: scenario: the phone drops at the channel switch and rejoins", "[ap]") {
    Sim sim;
    sim.c.boot(0);
    sim.run_to(120000);
    sim.clients(1);
    sim.run_to(150000);
    sim.get_ip();  // the AP moves to the router's channel
    sim.clients(0);
    sim.run_to(158000);
    sim.clients(1);
    sim.run_to(400000);
    CHECK(sim.downs.empty());
    CHECK(sim.ups.size() == 1);
}

TEST_CASE("AP: scenario: radio handover", "[ap]") {
    SECTION("station already down: time stopped counts") {
        Sim sim;
        sim.s = on(180);
        sim.c.boot(0);
        sim.run_to(100000);
        sim.c.stopped(sim.f.sta_has_ip);
        sim.f.stopped = true;
        sim.run_to(160000);
        CHECK(sim.ups.empty());
        sim.f.stopped = false;
        sim.c.restored(sim.now);
        sim.c.explicit_join(sim.now);
        sim.run_to(400000);
        REQUIRE(sim.ups.size() == 1);
        CHECK(sim.ups[0] == 180000);  // 20 s after restore
    }
    SECTION("past the delay while stopped: up at restore, not during the transfer") {
        Sim sim;
        sim.c.boot(0);
        sim.run_to(100000);
        sim.c.stopped(false);
        sim.f.stopped = true;
        sim.run_to(300000);
        CHECK(sim.ups.empty());
        sim.f.stopped = false;
        sim.c.restored(sim.now);
        sim.run_to(300000);
        REQUIRE(sim.ups.size() == 1);
        CHECK(sim.ups[0] == 300000);
    }
    SECTION("station up at the handover, router gone: the clock restarts at restore") {
        Sim sim;
        sim.c.boot(0);
        sim.get_ip();
        sim.run_to(50000);
        sim.c.stopped(true);
        sim.f.stopped = true;
        sim.lose_ip();  // esp_wifi_stop takes the IP with it
        sim.run_to(250000);
        sim.f.stopped = false;
        sim.c.restored(sim.now);
        sim.c.explicit_join(sim.now);
        sim.run_to(600000);
        REQUIRE(sim.ups.size() == 1);
        CHECK(sim.ups[0] == 370000);
    }
}

TEST_CASE("AP: scenario: forgetting the network brings the AP up at once", "[ap]") {
    Sim sim;
    sim.c.boot(0);
    sim.get_ip();
    sim.run_to(80000);
    sim.f.provisioned = false;
    sim.lose_ip();
    REQUIRE(sim.ups.size() == 1);
    CHECK(sim.ups[0] == 80000);
}

TEST_CASE("AP: scenario: a flapping station IP can't bounce the AP", "[ap]") {
    struct Pattern {
        uint64_t up_ms, down_ms;
    };
    for (const Pattern p : {Pattern{5000, 5000}, Pattern{5000, 31000}, Pattern{31000, 31000}, Pattern{40000, 5000},
                            Pattern{1000, 30000}}) {
        INFO(p.up_ms << "/" << p.down_ms);
        Sim sim;
        sim.s = on(kMinDelayS);
        sim.c.boot(0);
        sim.get_ip();
        uint64_t t = 0;
        while (t < 3600000) {
            t += p.up_ms;
            sim.run_to(t);
            sim.lose_ip();
            t += p.down_ms;
            sim.run_to(t);
            sim.get_ip();
        }
        for (size_t i = 1; i < sim.ups.size(); ++i) CHECK(sim.ups[i] - sim.ups[i - 1] >= 30000);
    }
}

// ---- seeded invariant fuzz -----------------------------------------------------------------

TEST_CASE("AP: seeded invariant fuzz", "[ap]") {
    std::mt19937_64 rng(0xDF032A9);
    const auto pick = [&](uint64_t n) { return std::uniform_int_distribution<uint64_t>(0, n - 1)(rng); };
    uint64_t checks = 0, paused = 0, bringups = 0, takedowns = 0;
    bool seen[8] = {};  // every Why

    for (int run = 0; run < 2000; ++run) {
        Controller c;
        Settings s;
        s.enabled = pick(4) != 0;
        s.delay_s = static_cast<uint32_t>(kMinDelayS + pick(kMaxDelayS - kMinDelayS + 1));
        Facts f = facts(pick(3) != 0, false, false, 0);
        uint64_t now = pick(1000000);
        c.boot(now);
        Decision d = c.step(s, f, now);

        for (int ev = 0; ev < 300; ++ev) {
            switch (pick(14)) {
                case 0: now += pick(300000); break;
                case 1:
                    if (d.next_eval_ms != kNever) now = d.next_eval_ms;
                    break;
                case 2:
                    if (d.next_eval_ms != kNever) now = d.next_eval_ms - 1;
                    break;
                case 3: s.enabled = !s.enabled; break;
                case 4: s.delay_s = static_cast<uint32_t>(kMinDelayS + pick(kMaxDelayS - kMinDelayS + 1)); break;
                case 5:
                    f.provisioned = !f.provisioned;
                    if (!f.provisioned && f.sta_has_ip) {
                        f.sta_has_ip = false;
                        c.ip_lost(now);
                    }
                    break;
                case 6:
                    if (f.sta_has_ip || pick(2)) {
                        f.sta_has_ip = false;
                        c.ip_lost(now);
                    }
                    break;
                case 7:
                    if (f.provisioned && !f.stopped) {
                        f.sta_has_ip = true;
                        c.ip_got();
                    }
                    break;
                case 8: f.clients = f.ap_up && !f.stopped ? static_cast<uint8_t>(pick(kMaxClients + 1)) : 0; break;
                case 9:
                    if (!f.stopped) {
                        c.stopped(f.sta_has_ip);
                        f.stopped = true;
                        f.clients = 0;
                        if (f.sta_has_ip) {
                            f.sta_has_ip = false;
                            if (pick(2)) c.ip_lost(now);
                        }
                    }
                    break;
                case 10:
                    if (f.stopped) {
                        f.stopped = false;
                        c.restored(now);
                        c.explicit_join(now);
                    }
                    break;
                case 11: c.explicit_join(now); break;
                case 12:
                    if (!f.stopped && f.ap_up && pick(4) == 0) {  // the driver drops the AP by itself
                        f.ap_up = false;
                        f.clients = 0;
                    }
                    break;
                default: break;
            }

            d = c.step(s, f, now);
            INFO("run " << run << " event " << ev << " now " << now);
            if (!f.stopped && !s.enabled) CHECK_FALSE(d.want_up);
            if (f.stopped) {
                CHECK(d.action == Action::None);
                CHECK_FALSE(d.pause_sta);
            }
            if (s.enabled && !f.stopped && !f.sta_has_ip && f.provisioned && !f.ap_up) {
                const uint64_t since = c.sta_down_since_ms();
                const bool elapsed = since != kNever && now >= since && now - since >= uint64_t{s.delay_s} * 1000;
                CHECK(d.want_up == elapsed);
            }
            CHECK((d.next_eval_ms == kNever || d.next_eval_ms > now));
            CHECK_FALSE(d.probe_now);

            // Soundness: nothing changes on its own before next_eval_ms (or ever, for kNever).
            const uint64_t later = d.next_eval_ms == kNever ? now + 1000000000 : d.next_eval_ms - 1;
            for (const uint64_t t : {now, later}) {
                Controller copy = c;
                const Decision again = copy.step(s, f, t);
                CHECK(again.want_up == d.want_up);
                CHECK(again.pause_sta == d.pause_sta);
            }
            ++checks;
            seen[static_cast<size_t>(d.why)] = true;
            paused += d.pause_sta;
            bringups += d.action == Action::BringUp;
            takedowns += d.action == Action::TakeDown;

            // The driver obeys, except for an occasional failed bring-up.
            if (d.action == Action::BringUp) {
                if (pick(5) == 0) {
                    c.bringup_failed(now);
                } else {
                    f.ap_up = true;
                }
            } else if (d.action == Action::TakeDown) {
                f.ap_up = false;
                f.clients = 0;
            }
        }
    }
    CHECK(checks == 2000 * 300);
    // The walk reaches every rule, the pause and both actions.
    for (const bool s : seen) CHECK(s);
    CHECK(paused > 1000);
    CHECK(bringups > 1000);
    CHECK(takedowns > 1000);
}
