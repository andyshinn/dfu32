// SPDX-License-Identifier: MIT
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>
#include <vector>

#include "dfu32_core/api_core.hpp"
#include "dfu32_core/json.hpp"
#include "dfu32_core/target_table.hpp"

using namespace dfu32::api;

namespace {

std::vector<uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

}  // namespace

TEST_CASE("API: route matching", "[api]") {
    CHECK(match_route("/api/v1/status").route == Route::Status);
    CHECK(match_route("/api/v1/status/").route == Route::Status);
    CHECK(match_route("/api/v1/status?x=1").route == Route::Status);
    CHECK(match_route("/api/v1/claim").route == Route::Claim);
    CHECK(match_route("/api/v1/targets").route == Route::Targets);
    CHECK(match_route("/api/v1/staging").route == Route::Staging);
    CHECK(match_route("/api/v1/logs?since=42").route == Route::Logs);
    CHECK(match_route("/api/v1/config").route == Route::Config);
    CHECK(match_route("/api/v1/ble/scan").route == Route::BleScan);
    CHECK(match_route("/api/v1/system/reboot").route == Route::Reboot);
    CHECK(match_route("/api/v1/system/token/rotate").route == Route::TokenRotate);
    CHECK(match_route("/api/v1/system/token/clear").route == Route::TokenClear);
    CHECK(match_route("/api/v1/system/ota").route == Route::SystemOta);
    CHECK(match_route("/api/v1/jobs/current/abort").route == Route::JobAbort);
    CHECK(match_route("/api/v1/debug/fault").route == Route::DebugFault);
    CHECK(match_route("/api/v1/wifi").route == Route::Wifi);
    CHECK(match_route("/api/v1/wifi/scan").route == Route::WifiScan);

    auto t = match_route("/api/v1/targets/attic-repeater");
    CHECK(t.route == Route::Target);
    CHECK(t.id == "attic-repeater");
    CHECK(match_route("/api/v1/targets/attic/check").route == Route::TargetCheck);
    CHECK(match_route("/api/v1/targets/attic/stage").route == Route::TargetStage);
    CHECK(match_route("/api/v1/targets/attic/flash").route == Route::TargetFlash);
    t = match_route("/api/v1/targets/attic/runs");
    CHECK(t.route == Route::TargetRuns);
    CHECK(t.id == "attic");
    t = match_route("/api/v1/targets/garage-2/default");
    CHECK(t.route == Route::TargetDefault);
    CHECK(t.id == "garage-2");
    CHECK(match_route("/api/v1/targets/attic/default/").route == Route::TargetDefault);
    CHECK(match_route("/api/v1/targets/attic/default?x=1").route == Route::TargetDefault);
    // A target may itself be called "default".
    t = match_route("/api/v1/targets/default");
    CHECK(t.route == Route::Target);
    CHECK(t.id == "default");

    auto r = match_route("/api/v1/runs/17");
    CHECK(r.route == Route::Run);
    CHECK(r.seq == 17);

    for (const char* bad : {"/", "/api/v1/", "/api/v1", "/api/v2/status", "/api/v1/statuses", "/api/v1//status",
                            "/api/v1/targets/attic/erase", "/api/v1/targets/attic/check/x", "/api/v1/runs/0",
                            "/api/v1/runs/x", "/api/v1/runs/007", "/api/v1/runs/0x10", "/api/v1/runs/99999999999",
                            "/api/v1/jobs/1/abort", "/api/v1/targets//check", "/api/v1/a/b/c/d/e", "/api/v1/wifi/x",
                            "/api/v1/wifi/scan/1", "/api/v1/wifiscan", "/api/v1/targets//default",
                            "/api/v1/targets/attic/default/x", "/api/v1/default"}) {
        INFO(bad);
        CHECK(match_route(bad).route == Route::NotFound);
        CHECK(match_route(bad).id.empty());
    }
}

TEST_CASE("API: methods and public routes", "[api]") {
    CHECK(method_allowed(Route::Status, Method::Get));
    CHECK_FALSE(method_allowed(Route::Status, Method::Post));
    CHECK(method_allowed(Route::Target, Method::Put));
    CHECK(method_allowed(Route::Target, Method::Delete));
    CHECK_FALSE(method_allowed(Route::Target, Method::Post));
    CHECK(method_allowed(Route::TargetFlash, Method::Post));
    CHECK_FALSE(method_allowed(Route::TargetFlash, Method::Get));
    CHECK(method_allowed(Route::TargetDefault, Method::Post));
    CHECK_FALSE(method_allowed(Route::TargetDefault, Method::Get));
    CHECK_FALSE(method_allowed(Route::TargetDefault, Method::Put));
    CHECK_FALSE(method_allowed(Route::TargetDefault, Method::Delete));
    CHECK_FALSE(is_public(Route::TargetDefault));
    CHECK(method_allowed(Route::Staging, Method::Delete));
    CHECK_FALSE(method_allowed(Route::Staging, Method::Put));
    CHECK(method_allowed(Route::Staging, Method::Post));  // upload
    CHECK(method_allowed(Route::Config, Method::Put));
    CHECK_FALSE(method_allowed(Route::NotFound, Method::Get));
    CHECK_FALSE(method_allowed(Route::Logs, Method::Other));

    CHECK(is_public(Route::Status));
    CHECK(is_public(Route::Claim));
    CHECK_FALSE(is_public(Route::Targets));
    CHECK_FALSE(is_public(Route::TokenRotate));
    CHECK_FALSE(is_public(Route::TokenClear));
    CHECK(method_allowed(Route::TokenClear, Method::Post));
    CHECK_FALSE(method_allowed(Route::TokenClear, Method::Get));
    CHECK_FALSE(is_public(Route::SystemOta));
    CHECK(method_allowed(Route::SystemOta, Method::Post));
    CHECK_FALSE(method_allowed(Route::SystemOta, Method::Get));

    CHECK(method_allowed(Route::Wifi, Method::Put));
    for (Method m : {Method::Get, Method::Post, Method::Delete, Method::Other}) {
        CHECK_FALSE(method_allowed(Route::Wifi, m));
    }
    CHECK(method_allowed(Route::WifiScan, Method::Post));
    for (Method m : {Method::Get, Method::Put, Method::Delete, Method::Other}) {
        CHECK_FALSE(method_allowed(Route::WifiScan, m));
    }
    CHECK_FALSE(is_public(Route::Wifi));
    CHECK_FALSE(is_public(Route::WifiScan));
}

TEST_CASE("API: query parameters", "[api]") {
    CHECK(query_param("/api/v1/logs?since=42", "since") == "42");
    CHECK(query_param("/api/v1/logs?a=1&since=7&b", "since") == "7");
    CHECK(query_param("/api/v1/logs?a=1&b", "b").empty());
    CHECK(query_param("/api/v1/logs?sinceX=1", "since").empty());
    CHECK(query_param("/api/v1/logs", "since").empty());
}

TEST_CASE("API: %-decoded query text", "[api]") {
    std::string v = "stale";
    CHECK(query_text("/x?version=v1.18.0%2B2%20rc&b=1", "version", v));
    CHECK(v == "v1.18.0+2 rc");
    CHECK(query_text("/x?name=My+Repeater%2fzip", "name", v));
    CHECK(v == "My Repeater/zip");
    CHECK(query_text("/x?name=%C3%A9", "name", v));
    CHECK(v == "\xC3\xA9");
    CHECK(query_text("/x", "name", v));
    CHECK(v.empty());
    CHECK_FALSE(query_text("/x?name=a%2", "name", v));
    CHECK_FALSE(query_text("/x?name=a%", "name", v));
    CHECK_FALSE(query_text("/x?name=a%zz", "name", v));
}

TEST_CASE("API: staging upload query", "[api][upload]") {
    StagingUpload u;
    REQUIRE(parse_staging_upload("/api/v1/staging?dat=5200ffffffffffff0100b6003412&version=v1.18.0-81dbae5", u).empty());
    CHECK(u.dat == std::vector<uint8_t>{0x52, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0xB6, 0x00, 0x34, 0x12});
    CHECK(u.version == "v1.18.0-81dbae5");
    CHECK(u.board.empty());
    CHECK_FALSE(u.has_sha256);
    CHECK(u.source == "upload");
    CHECK(u.target.empty());

    REQUIRE(parse_staging_upload("/api/v1/staging?dat=00&version=v1&target=garage-2", u).empty());
    CHECK(u.target == "garage-2");
    REQUIRE(parse_staging_upload("/api/v1/staging?dat=00&version=v1&target=", u).empty());
    CHECK(u.target.empty());

    const std::string sha(64, 'a');
    REQUIRE(parse_staging_upload("/api/v1/staging?version=v1&dat=00AA&board=rak4631&sha256=" + sha +
                                     "&name=RAK_4631_repeater-v1.18.0.zip",
                                 u)
                .empty());
    CHECK(u.dat == std::vector<uint8_t>{0x00, 0xAA});
    CHECK(u.board == "rak4631");
    CHECK(u.has_sha256);
    CHECK(u.sha256[0] == 0xAA);
    CHECK(u.source == "upload:RAK_4631_repeater-v1.18.0.zip");

    // A long name is clipped to the header's 255 bytes, never mid-character.
    std::string name;
    for (int i = 0; i < 200; ++i) name += "%C3%A9";
    REQUIRE(parse_staging_upload("/s?dat=00&version=v&name=" + name, u).empty());
    CHECK(u.source.size() <= 255);
    CHECK(u.source.size() % 2 == 1);  // "upload:" (7) + whole 2-byte characters

    const auto refused = [&](const std::string& q, const char* why) {
        INFO(q);
        const std::string e = parse_staging_upload("/api/v1/staging" + q, u);
        CHECK_THAT(e, Catch::Matchers::ContainsSubstring(why));
    };
    refused("", "dat is required");
    refused("?version=v1", "dat is required");
    refused("?dat=abc&version=v1", "1-64 bytes");
    refused("?dat=" + std::string(130, 'a') + "&version=v1", "1-64 bytes");
    refused("?dat=zz&version=v1", "hex");
    refused("?dat=00", "version is required");
    refused("?dat=00&version=" + std::string(64, 'v'), "version is required");
    refused("?dat=00&version=a%0Ab", "control");
    refused("?dat=00&version=v1&board=" + std::string(32, 'b'), "board");
    refused("?dat=00&version=v1&board=a+b", "board");
    refused("?dat=00&version=v1&sha256=abc", "sha256");
    refused("?dat=00&version=v1&name=a%00b", "control");
    refused("?dat=00&version=v%zz", "%-escape");
    refused("?dat=00&version=v1&target=Attic", "target: id may contain only");
    refused("?dat=00&version=v1&target=a%2Fb", "target: id may contain only");
    refused("?dat=00&version=v1&target=" + std::string(32, 't'), "target: id longer");
    refused("?dat=00&version=v1&target=a%zz", "target: malformed");
}

TEST_CASE("API: which target an upload is checked against", "[api][upload]") {
    using dfu32::TargetConfig;
    using dfu32::TargetTable;
    const auto make = [](const std::string& id, const std::string& board) {
        TargetConfig t;
        t.id = id;
        t.board = board;
        return t;
    };

    TargetTable none(5);
    CHECK(staging_target_for(none, "rak4631") == nullptr);
    CHECK(staging_target_for(none, "") == nullptr);

    TargetTable table(5);
    REQUIRE(table.put(make("attic", "heltec_t114")).status == TargetTable::PutStatus::Created);
    REQUIRE(table.put(make("garage", "rak4631")).status == TargetTable::PutStatus::Created);
    REQUIRE(table.put(make("shed", "RAK4631")).status == TargetTable::PutStatus::Created);
    REQUIRE(table.default_id() == "attic");

    // The default target when its board accepts the image.
    const auto* st = staging_target_for(table, "heltec_t114");
    REQUIRE(st != nullptr);
    CHECK(st->id() == "attic");
    // An image without a board, or any image when the default has none: the default.
    st = staging_target_for(table, "");
    REQUIRE(st != nullptr);
    CHECK(st->id() == "attic");
    // Otherwise the first target in slot order whose board accepts it.
    st = staging_target_for(table, "rak4631");
    REQUIRE(st != nullptr);
    CHECK(st->id() == "garage");
    // The default wins over an earlier slot.
    dfu32::TargetWrites w;
    REQUIRE(table.set_default("shed", w));
    st = staging_target_for(table, "rak4631");
    REQUIRE(st != nullptr);
    CHECK(st->id() == "shed");
    // No target takes it.
    CHECK(staging_target_for(table, "xiao_nrf52") == nullptr);

    // A target with no board accepts any image.
    REQUIRE(table.put(make("bench", "")).status == TargetTable::PutStatus::Created);
    st = staging_target_for(table, "xiao_nrf52");
    REQUIRE(st != nullptr);
    CHECK(st->id() == "bench");
}

TEST_CASE("API: base64url (RFC 4648 test vectors, url alphabet, no padding)", "[api]") {
    CHECK(base64url_encode(bytes("")) == "");
    CHECK(base64url_encode(bytes("f")) == "Zg");
    CHECK(base64url_encode(bytes("fo")) == "Zm8");
    CHECK(base64url_encode(bytes("foo")) == "Zm9v");
    CHECK(base64url_encode(bytes("foob")) == "Zm9vYg");
    CHECK(base64url_encode(bytes("fooba")) == "Zm9vYmE");
    CHECK(base64url_encode(bytes("foobar")) == "Zm9vYmFy");
    const std::array<uint8_t, 3> url{0xFB, 0xFF, 0xBF};
    CHECK(base64url_encode(url) == "-_-_");
    std::array<uint8_t, 32> token{};
    CHECK(base64url_encode(token).size() == 43);
}

TEST_CASE("API: constant-time compare and bearer header", "[api]") {
    CHECK(ct_equal(bytes("abc"), bytes("abc")));
    CHECK_FALSE(ct_equal(bytes("abc"), bytes("abd")));
    CHECK_FALSE(ct_equal(bytes("abc"), bytes("abcd")));
    CHECK(ct_equal(bytes(""), bytes("")));

    CHECK(bearer_token("Bearer abc-_") == "abc-_");
    CHECK(bearer_token("bearer  abc ") == "abc");
    CHECK(bearer_token("BEARER abc") == "abc");
    CHECK(bearer_token("Basic abc").empty());
    CHECK(bearer_token("Bearer").empty());
    CHECK(bearer_token("Bearer ").empty());
    CHECK(bearer_token("").empty());
}

TEST_CASE("API: auth throttle", "[api]") {
    AuthThrottle t;
    uint64_t now = 1000;
    for (int i = 0; i < 4; ++i) {
        CHECK(t.try_attempt(now += 100));
        t.record_failure(now);
    }
    CHECK_FALSE(t.penalised(now));
    CHECK(t.try_attempt(now));  // no penalty yet: every attempt is checked
    CHECK(t.try_attempt(now));
    t.record_failure(now += 100);  // 5th failure
    const uint64_t start = now;
    CHECK(t.penalised(now));
    CHECK(t.try_attempt(now));         // one check per second...
    CHECK_FALSE(t.try_attempt(now));   // ...the rest are refused, not delayed
    CHECK_FALSE(t.try_attempt(now + 999));
    CHECK(t.try_attempt(now + 1000));
    CHECK_FALSE(t.try_attempt(now + 1500));
    CHECK(t.penalised(start + AuthThrottle::kPenaltyMs - 1));
    CHECK_FALSE(t.penalised(start + AuthThrottle::kPenaltyMs));
    CHECK(t.try_attempt(start + AuthThrottle::kPenaltyMs));
    CHECK(t.try_attempt(start + AuthThrottle::kPenaltyMs));

    // Failures spread over more than a minute don't add up.
    AuthThrottle slow;
    uint64_t at = 0;
    for (int i = 0; i < 10; ++i) {
        slow.record_failure(at);
        at += 20000;
        if (i % 3 == 2) at += 60000;
    }
    CHECK_FALSE(slow.penalised(at));
}

TEST_CASE("API: device config", "[api]") {
    DeviceConfig c;
    CHECK(validate(c).empty());
    CHECK(apply_json(R"({"hostname":"attic-dfu","ntp_server":"time.lan","wifi_off_dfu":false,"dfu_deadline_max_s":900})",
                     c)
              .empty());
    CHECK(c.hostname == "attic-dfu");
    CHECK(c.ntp_server == "time.lan");
    CHECK_FALSE(c.wifi_off_dfu);
    CHECK(c.dfu_deadline_max_s == 900);
    CHECK(apply_json(R"({"hostname":""})", c).empty());  // back to the default name
    CHECK(c.hostname.empty());
    CHECK(c.ntp_server == "time.lan");

    const DeviceConfig before = c;
    CHECK_FALSE(apply_json(R"({"hostname":"Bad_Name"})", c).empty());
    CHECK_FALSE(apply_json(R"({"ntp_server":"a..b"})", c).empty());
    CHECK_FALSE(apply_json(R"({"ntp_server":"-a.b"})", c).empty());
    CHECK_FALSE(apply_json(R"({"ntp_server":"a b"})", c).empty());
    CHECK_FALSE(apply_json(R"({"dfu_deadline_max_s":60})", c).empty());
    CHECK_FALSE(apply_json(R"({"wifi_off_dfu":"no"})", c).empty());
    CHECK(apply_json(R"({"prn":8})", c) == "unknown field 'prn'");
    CHECK(c.ntp_server == before.ntp_server);
    CHECK(c.dfu_deadline_max_s == before.dfu_deadline_max_s);

    CHECK(validate_ntp_server("192.168.1.10").empty());
    CHECK(validate_ntp_server("pool.ntp.org").empty());
    CHECK_FALSE(validate_ntp_server("pool.ntp.org.").empty());

    dfu32::json::ObjectWriter w;
    write_json(w, c);
    CHECK(w.str() == R"({"hostname":"","ntp_server":"time.lan","wifi_off_dfu":false,"dfu_deadline_max_s":900,)"
                     R"("ap_enabled":false,"ap_delay_s":120,"ap_idle_s":1800,"ap_password_set":false})");
}

TEST_CASE("API: device config, access-point settings", "[api]") {
    DeviceConfig c;
    CHECK_FALSE(c.ap_enabled);
    CHECK(c.ap_delay_s == 120);
    CHECK(c.ap_password.empty());

    CHECK(apply_json(R"({"ap_enabled":true,"ap_delay_s":30,"ap_password":"s3cret pass"})", c).empty());
    CHECK(c.ap_enabled);
    CHECK(c.ap_delay_s == 30);
    CHECK(c.ap_password == "s3cret pass");
    CHECK(apply_json(R"({"ap_delay_s":1800})", c).empty());
    CHECK(c.ap_delay_s == 1800);
    CHECK(c.ap_password == "s3cret pass");  // absent keeps it

    // Write-only: GET /config never carries the password.
    dfu32::json::ObjectWriter w;
    write_json(w, c);
    const std::string out = w.str();
    CHECK(out.find("s3cret") == std::string::npos);
    CHECK_THAT(out, Catch::Matchers::EndsWith(
                        R"("ap_enabled":true,"ap_delay_s":1800,"ap_idle_s":1800,"ap_password_set":true})"));

    const DeviceConfig before = c;
    const auto refused = [&](const char* body) {
        INFO(body);
        CHECK_FALSE(apply_json(body, c).empty());
        CHECK(c.ap_enabled == before.ap_enabled);
        CHECK(c.ap_delay_s == before.ap_delay_s);
        CHECK(c.ap_password == before.ap_password);
        CHECK(c.ap_idle_s == before.ap_idle_s);
    };
    refused(R"({"ap_delay_s":29})");
    refused(R"({"ap_delay_s":1801})");
    refused(R"({"ap_delay_s":-1})");
    refused(R"({"ap_delay_s":1.5})");
    refused(R"({"ap_delay_s":"120"})");
    refused(R"({"ap_password":"1234567"})");
    refused(R"({"ap_password":"1234567890123456789012345678901234567890123456789012345678901234"})");
    refused(R"({"ap_password":"line
break"})");
    refused(R"({"ap_password":12345678})");
    refused(R"({"ap_enabled":"true"})");
    refused(R"({"ap_enabled":false,"ap_delay_s":29})");  // nothing applied from a refused body
    CHECK(apply_json(R"({"ap_password_set":false})", c) == "unknown field 'ap_password_set'");
    CHECK(apply_json(R"({"ap_delay_s":29})", c) == "ap_delay_s must be 30-1800");

    CHECK(apply_json(R"({"ap_password":""})", c).empty());  // open
    CHECK(c.ap_password.empty());

    DeviceConfig bad;
    bad.ap_delay_s = 10;
    CHECK_FALSE(validate(bad).empty());
    bad.ap_delay_s = 120;
    bad.ap_password = "short";
    CHECK_FALSE(validate(bad).empty());
    bad.ap_password.clear();
    bad.ap_idle_s = 299;
    CHECK(validate(bad) == "ap_idle_s must be 0 (never) or 300-86400");
    bad.ap_idle_s = 0;
    CHECK(validate(bad).empty());
}

TEST_CASE("API: device config, access-point idle limit", "[api]") {
    DeviceConfig c;
    CHECK(c.ap_idle_s == dfu32::ap::kDefaultIdleS);
    CHECK(c.ap_idle_s == 1800);

    // Boundaries: 0 = never, else 300-86400 s (SPEC.md §9.5).
    for (const uint32_t ok : {0u, 300u, 301u, 1800u, 86400u}) {
        INFO(ok);
        CHECK(apply_json(R"({"ap_idle_s":)" + std::to_string(ok) + "}", c).empty());
        CHECK(c.ap_idle_s == ok);
        dfu32::json::ObjectWriter w;
        write_json(w, c);
        CHECK(w.str().find(R"("ap_idle_s":)" + std::to_string(ok) + ",") != std::string::npos);
    }

    CHECK(apply_json(R"({"ap_idle_s":600})", c).empty());
    const DeviceConfig before = c;
    const auto refused = [&](const char* body) {
        INFO(body);
        CHECK(apply_json(body, c) == "ap_idle_s must be 0 (never) or 300-86400");
        CHECK(c.ap_idle_s == before.ap_idle_s);
        CHECK(c.ap_enabled == before.ap_enabled);
    };
    refused(R"({"ap_idle_s":1})");
    refused(R"({"ap_idle_s":299})");
    refused(R"({"ap_idle_s":86401})");
    refused(R"({"ap_idle_s":-1})");
    refused(R"({"ap_idle_s":4294967296})");  // would wrap to 0 = never as a u32
    refused(R"({"ap_idle_s":4294967295})");
    refused(R"({"ap_idle_s":1.5})");
    refused(R"({"ap_idle_s":"600"})");
    refused(R"({"ap_idle_s":null})");
    refused(R"({"ap_idle_s":false})");
    refused(R"({"ap_enabled":true,"ap_idle_s":299})");  // nothing applied from a refused body

    // Round trip: GET /config's value is accepted back by PUT unchanged.
    dfu32::json::ObjectWriter w;
    write_json(w, c);
    dfu32::json::Doc doc;
    REQUIRE(doc.parse(w.str()));
    int64_t idle = 0;
    REQUIRE(doc.root().get("ap_idle_s").integer(idle));
    DeviceConfig back;
    CHECK(apply_json(R"({"ap_idle_s":)" + std::to_string(idle) + "}", back).empty());
    CHECK(back.ap_idle_s == c.ap_idle_s);
}

TEST_CASE("API: WiFi station request body", "[api][wifi]") {
    std::string ssid = "keep", pw = "keep-pass";
    CHECK(parse_wifi_request(R"({"ssid":"Cafe"})", ssid, pw).empty());
    CHECK(ssid == "Cafe");
    CHECK(pw.empty());
    CHECK(parse_wifi_request(R"({"ssid":"Home Net","password":"correct horse"})", ssid, pw).empty());
    CHECK(ssid == "Home Net");
    CHECK(pw == "correct horse");

    ssid = "keep";
    pw = "keep-pass";
    const auto refused = [&](std::string_view body) {
        INFO(body);
        CHECK_FALSE(parse_wifi_request(body, ssid, pw).empty());
        CHECK(ssid == "keep");
        CHECK(pw == "keep-pass");
    };
    refused(R"({"password":"correct horse"})");
    refused(R"({"ssid":42})");
    refused(R"({"ssid":"Home","password":"short"})");
    refused(R"({"ssid":"Home","password":7})");
    refused(R"({"ssid":""})");
    refused(R"({"ssid":"Home","psk":"x"})");
    refused(R"(["Home"])");
    refused("");
    CHECK(parse_wifi_request(R"({"password":"x"})", ssid, pw) == "ssid is required");
    CHECK(parse_wifi_request(R"({"ssid":"a","bssid":"x"})", ssid, pw) == "unknown field 'bssid'");
}

TEST_CASE("API: WiFi station request body, ssid_hex", "[api][wifi]") {
    std::string ssid, pw = "keep-pass";
    // Raw bytes, decoded as is: invalid UTF-8 survives (SPEC.md §9.5).
    CHECK(parse_wifi_request(R"({"ssid_hex":"61ff01","password":"correct horse"})", ssid, pw).empty());
    CHECK(ssid == "a\xFF\x01");
    CHECK(pw == "correct horse");
    CHECK(parse_wifi_request(R"({"ssid_hex":"ff"})", ssid, pw).empty());  // open
    CHECK(ssid == "\xFF");
    CHECK(pw.empty());
    CHECK(parse_wifi_request(R"({"ssid_hex":"C3a9Ff"})", ssid, pw).empty());  // either case
    CHECK(ssid == "\xC3\xA9\xFF");
    const std::string max(64, 'f');  // 32 bytes
    CHECK(parse_wifi_request(R"({"ssid_hex":")" + max + R"("})", ssid, pw).empty());
    CHECK(ssid == std::string(32, '\xFF'));

    ssid = "keep";
    pw = "keep-pass";
    const auto refused = [&](const std::string& body, std::string_view why) {
        INFO(body);
        CHECK(parse_wifi_request(body, ssid, pw) == why);
        CHECK(ssid == "keep");
        CHECK(pw == "keep-pass");
    };
    constexpr std::string_view kBadHex = "ssid_hex must be 1-32 bytes as hex";
    refused(R"({"ssid_hex":""})", kBadHex);
    refused(R"({"ssid_hex":"6"})", kBadHex);      // odd length
    refused(R"({"ssid_hex":"616"})", kBadHex);    // odd length
    refused(R"({"ssid_hex":"6g"})", kBadHex);     // not hex
    refused(R"({"ssid_hex":"0x61"})", kBadHex);
    refused(R"({"ssid_hex":"61 62"})", kBadHex);
    refused(R"({"ssid_hex":")" + max + R"(00"})", kBadHex);  // 33 bytes
    refused(R"({"ssid_hex":6162})", kBadHex);
    refused(R"({"ssid_hex":null})", kBadHex);
    // NUL: net::set_credentials() can't store it, so it is refused here, not after the 202.
    constexpr std::string_view kNul = "ssid_hex must not contain 00 (NUL)";
    refused(R"({"ssid_hex":"00"})", kNul);
    refused(R"({"ssid_hex":"61ff00"})", kNul);
    refused(R"({"ssid_hex":"610062"})", kNul);
    refused(R"({"ssid":"Home","ssid_hex":"486f6d65"})", "give ssid or ssid_hex, not both");
    refused(R"({"ssid_hex":"486f6d65","ssid":""})", "give ssid or ssid_hex, not both");
    refused(R"({"ssid_hex":"486f6d65","password":"short"})",
            "password must be 8-63 printable ASCII characters or 64 hex digits");
}

TEST_CASE("API: WiFi scan view", "[api][wifi]") {
    CHECK(sanitize_ssid("plain") == "plain");
    CHECK(sanitize_ssid("caf\xC3\xA9 \xF0\x9F\x93\xB6") == "caf\xC3\xA9 \xF0\x9F\x93\xB6");  // valid UTF-8 kept
    CHECK(sanitize_ssid("a\xFF" "b") == "a\xEF\xBF\xBD" "b");
    CHECK(sanitize_ssid("\xC3") == "\xEF\xBF\xBD");                       // truncated sequence
    CHECK(sanitize_ssid("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");     // overlong
    CHECK(sanitize_ssid("\xED\xA0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");  // surrogate
    CHECK(sanitize_ssid(std::string("a\0b", 3)) == "a\xEF\xBF\xBD" "b");

    const std::string nasty = "\"q\" \\ \x01\x1F <script>alert(1)</script>";
    const std::vector<WifiNet> nets = {{nasty, -40, true}, {"open \xFF", -80, false}};
    dfu32::json::ObjectWriter w;
    write_wifi_scan(w, nets);
    const std::string text = w.str();
    dfu32::json::Doc doc;
    REQUIRE(doc.parse(text));
    const auto list = doc.root().get("networks").items();
    REQUIRE(list.size() == 2);
    CHECK(list[0].get("ssid").str() == nasty);
    int64_t rssi = 0;
    CHECK(list[0].get("rssi").integer(rssi));
    CHECK(rssi == -40);
    CHECK(list[0].get("secured").boolean());
    CHECK(list[1].get("ssid").str() == "open \xEF\xBF\xBD");
    CHECK_FALSE(list[1].get("secured").boolean(true));

    CHECK(list[0].get("ssid_hex").missing());  // escaped by the writer, nothing lost
    CHECK(list[1].get("ssid_hex").str() == "6f70656e20ff");

    dfu32::json::ObjectWriter empty;
    write_wifi_scan(empty, {});
    CHECK(empty.str() == R"({"networks":[]})");
}

TEST_CASE("API: BLE scan view", "[api]") {
    CHECK(std::string(classify_dfu("RAK4631_OTA", false)) == "app_buttonless");
    CHECK(std::string(classify_dfu("t096_ota", false)) == "app_buttonless");
    CHECK(std::string(classify_dfu("AdaDFU", false)) == "bootloader");
    CHECK(std::string(classify_dfu("XIAO DFU", true)) == "bootloader");
    CHECK(std::string(classify_dfu("", true)) == "bootloader");
    CHECK(classify_dfu("Galaxy Buds", false) == nullptr);
    CHECK(classify_dfu("", false) == nullptr);

    CHECK(mac_plus_one("F4:1C:36:A7:33:95") == "F4:1C:36:A7:33:96");
    CHECK(mac_plus_one("f4:1c:36:a7:33:ff") == "F4:1C:36:A7:33:00");  // wraps, no carry
    CHECK(mac_plus_one("nope").empty());

    CHECK(matches_target("F4:1C:36:A7:33:95", "f4:1c:36:a7:33:95"));
    CHECK(matches_target("F4:1C:36:A7:33:96", "F4:1C:36:A7:33:95"));
    CHECK_FALSE(matches_target("F4:1C:36:A7:33:97", "F4:1C:36:A7:33:95"));
    CHECK_FALSE(matches_target("F4:1C:36:A7:33:95", ""));

    dfu32::TargetTable table(5);
    CHECK(matching_target_id(table, "F4:1C:36:A7:33:95").empty());
    dfu32::TargetConfig a;
    a.id = "attic";
    a.mac = "F4:1C:36:A7:33:95";
    dfu32::TargetConfig b;
    b.id = "garage";
    b.mac = "F4:1C:36:A7:33:96";  // attic's MAC+1: attic still matches first
    dfu32::TargetConfig c;  // id-less legacy target, effective id "target"
    c.mac = "C2:4E:11:9A:03:7F";
    dfu32::TargetConfig d;
    d.id = "nomac";
    REQUIRE(table.put(d).status == dfu32::TargetTable::PutStatus::Created);
    REQUIRE(table.put(a).status == dfu32::TargetTable::PutStatus::Created);
    REQUIRE(table.put(b).status == dfu32::TargetTable::PutStatus::Created);
    REQUIRE(table.put(c).status == dfu32::TargetTable::PutStatus::Created);
    CHECK(matching_target_id(table, "f4:1c:36:a7:33:95") == "attic");
    CHECK(matching_target_id(table, "F4:1C:36:A7:33:96") == "attic");
    CHECK(matching_target_id(table, "F4:1C:36:A7:33:97") == "garage");
    CHECK(matching_target_id(table, "C2:4E:11:9A:03:80") == "target");
    CHECK(matching_target_id(table, "00:11:22:33:44:55").empty());
    CHECK(matching_target_id(table, "nope").empty());
}

TEST_CASE("API: scan and fault request bodies", "[api]") {
    ScanRequest s;
    CHECK(parse_scan_request("", s).empty());
    CHECK(s.duration_s == 10);
    CHECK_FALSE(s.dfu_only);
    CHECK(parse_scan_request(R"({"duration_s":3,"dfu_only":true})", s).empty());
    CHECK(s.duration_s == 3);
    CHECK(s.dfu_only);
    CHECK(parse_scan_request(R"({"duration_s":0})", s) == "duration_s must be 1-30");
    CHECK(parse_scan_request(R"({"duration_s":31})", s) == "duration_s must be 1-30");
    CHECK(parse_scan_request(R"({"dfu":true})", s) == "unknown field 'dfu'");
    CHECK_FALSE(parse_scan_request(R"({"dfu_only":"yes"})", s).empty());

    FaultRequest f;
    CHECK(parse_fault_request(R"({"restore_fail":true,"deadline_s":30})", f).empty());
    CHECK(f.restore_fail);
    CHECK(f.deadline_s == 30);
    CHECK(f.stall_s == 0);
    CHECK(parse_fault_request("{}", f).empty());
    CHECK_FALSE(f.restore_fail);
    CHECK_FALSE(parse_fault_request(R"({"stall_s":601})", f).empty());
    CHECK_FALSE(parse_fault_request(R"({"boom":1})", f).empty());
}

TEST_CASE("API: WiFi scan ssid_hex round trip", "[api][wifi]") {
    // Each name the scan had to sanitise comes back byte for byte through PUT /wifi's ssid_hex;
    // a clean one is not given ssid_hex and round-trips through ssid.
    const std::vector<std::string> raws = {
        "Home",
        "caf\xC3\xA9",
        std::string("a\0b", 3),
        std::string(1, '\0'),
        "\xFF\xFE",
        "\xC0\xAF",
        "\xED\xA0\x80",
        std::string(32, '\x80'),
        "\x01ctl",
    };
    std::vector<WifiNet> nets;
    for (const auto& r : raws) nets.push_back({r, -50, false});
    dfu32::json::ObjectWriter w;
    write_wifi_scan(w, nets);
    dfu32::json::Doc doc;
    REQUIRE(doc.parse(w.str()));
    const auto list = doc.root().get("networks").items();
    REQUIRE(list.size() == raws.size());
    for (size_t i = 0; i < raws.size(); ++i) {
        INFO(i);
        const bool lossy = sanitize_ssid(raws[i]) != raws[i];
        const auto hex = list[i].get("ssid_hex");
        CHECK(hex.missing() == !lossy);
        dfu32::json::ObjectWriter req;
        if (lossy) {
            const std::string h = hex.str();
            CHECK(h.size() == raws[i].size() * 2);
            CHECK(h.find_first_not_of("0123456789abcdef") == std::string::npos);  // lowercase
            req.add("ssid_hex", h);
        } else {
            req.add("ssid", list[i].get("ssid").str());
        }
        std::string ssid, pw;
        if (raws[i].find('\0') != std::string::npos) {
            // The scan never gives one (C strings), and the driver couldn't store it.
            CHECK(parse_wifi_request(req.str(), ssid, pw) == "ssid_hex must not contain 00 (NUL)");
            continue;
        }
        CHECK(parse_wifi_request(req.str(), ssid, pw).empty());
        CHECK(ssid == raws[i]);
    }
}
