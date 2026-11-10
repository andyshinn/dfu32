// SPDX-License-Identifier: MIT
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>
#include <vector>

#include "dfu32_core/api_core.hpp"
#include "dfu32_core/json.hpp"

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

    auto t = match_route("/api/v1/targets/attic-repeater");
    CHECK(t.route == Route::Target);
    CHECK(t.id == "attic-repeater");
    CHECK(match_route("/api/v1/targets/attic/check").route == Route::TargetCheck);
    CHECK(match_route("/api/v1/targets/attic/stage").route == Route::TargetStage);
    CHECK(match_route("/api/v1/targets/attic/flash").route == Route::TargetFlash);
    t = match_route("/api/v1/targets/attic/runs");
    CHECK(t.route == Route::TargetRuns);
    CHECK(t.id == "attic");

    auto r = match_route("/api/v1/runs/17");
    CHECK(r.route == Route::Run);
    CHECK(r.seq == 17);

    for (const char* bad : {"/", "/api/v1/", "/api/v1", "/api/v2/status", "/api/v1/statuses", "/api/v1//status",
                            "/api/v1/targets/attic/erase", "/api/v1/targets/attic/check/x", "/api/v1/runs/0",
                            "/api/v1/runs/x", "/api/v1/runs/007", "/api/v1/runs/0x10", "/api/v1/runs/99999999999",
                            "/api/v1/jobs/1/abort", "/api/v1/targets//check", "/api/v1/a/b/c/d/e"}) {
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
    CHECK(w.str() == R"({"hostname":"","ntp_server":"time.lan","wifi_off_dfu":false,"dfu_deadline_max_s":900})");
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
