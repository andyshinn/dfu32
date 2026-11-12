// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "dfu32_core/target_config.hpp"

using namespace dfu32;

TEST_CASE("Target config validation", "[target]") {
    TargetConfig t;
    CHECK(validate(t).empty());  // everything unset is valid
    CHECK(t.effective_id() == "target");

    t.id = "attic-repeater";
    t.board = "rak4631";
    t.mac = "f4:1c:36:a7:33:95";
    t.manifest_url = "http://fw.lan/rak/manifest.json";
    CHECK(validate(t).empty());
    CHECK(normalize_mac(t.mac) == "F4:1C:36:A7:33:95");
    CHECK(t.effective_id() == "attic-repeater");

    CHECK_FALSE(validate_target_id("Attic").empty());
    CHECK_FALSE(validate_target_id("a b").empty());
    CHECK_FALSE(validate_target_id(std::string(32, 'a')).empty());
    CHECK_FALSE(validate_board("rak 4631").empty());
    CHECK_FALSE(validate_board(std::string(32, 'b')).empty());
    CHECK_FALSE(validate_mac("F4:1C:36:A7:33").empty());
    CHECK_FALSE(validate_mac("F4-1C-36-A7-33-95").empty());
    CHECK_FALSE(validate_mac("G4:1C:36:A7:33:95").empty());
    CHECK(normalize_mac("nonsense").empty());
    CHECK_FALSE(validate_manifest_url("https://fw.lan/m.json").empty());
    CHECK_FALSE(validate_manifest_url("fw.lan/m.json").empty());
    CHECK_FALSE(validate_manifest_url("http://fw.lan/" + std::string(250, 'x')).empty());

    t.addr_type = 2;
    CHECK_FALSE(validate(t).empty());
}

TEST_CASE("Target config: size, hostname and strict number parsing", "[target]") {
    CHECK(validate_max_app_size(0).empty());
    CHECK(validate_max_app_size(802816).empty());
    CHECK_FALSE(validate_max_app_size(78).empty());
    CHECK_FALSE(validate_max_app_size(802817).empty());
    CHECK_FALSE(validate_max_app_size(32u * 1024 * 1024).empty());
    TargetConfig t;
    t.max_app_size = 78;
    CHECK_FALSE(validate(t).empty());

    uint32_t v = 0;
    CHECK(parse_u32("802816", v));
    CHECK(v == 802816);
    CHECK(parse_u32("0xC4000", v));
    CHECK(v == 0xC4000);
    CHECK(parse_u32("4294967295", v));
    CHECK_FALSE(parse_u32("4294967296", v));
    CHECK_FALSE(parse_u32("abc", v));
    CHECK_FALSE(parse_u32("78k", v));
    CHECK_FALSE(parse_u32("", v));
    CHECK_FALSE(parse_u32("0x", v));
    CHECK_FALSE(parse_u32("-1", v));

    CHECK(validate_hostname("dfu32-a1b2").empty());
    CHECK_FALSE(validate_hostname("").empty());
    CHECK_FALSE(validate_hostname("DFU32").empty());
    CHECK_FALSE(validate_hostname("-dfu").empty());
    CHECK_FALSE(validate_hostname("dfu-").empty());
    CHECK_FALSE(validate_hostname("dfu.local").empty());
    CHECK_FALSE(validate_hostname(std::string(32, 'a')).empty());
}

TEST_CASE("Target config: JSON round trip", "[target]") {
    TargetConfig t;
    t.id = "attic-repeater";
    t.name = "Attic \"north\" repeater";
    t.board = "rak4631";
    t.mac = "F4:1C:36:A7:33:95";
    t.addr_type = 0;
    t.manifest_url = "http://fw.lan/rak/manifest.json";
    t.max_app_size = 802816;
    TargetConfig back;
    REQUIRE(from_json(to_json(t), back));
    CHECK(back.id == t.id);
    CHECK(back.name == t.name);
    CHECK(back.board == t.board);
    CHECK(back.mac == t.mac);
    CHECK(back.addr_type == 0);
    CHECK(back.manifest_url == t.manifest_url);
    CHECK(back.max_app_size == 802816);

    TargetConfig partial;
    REQUIRE(from_json(R"({"board":"x","future_key":[1]})", partial));
    CHECK(partial.board == "x");
    CHECK(partial.addr_type == 1);
    CHECK_FALSE(from_json("{", partial));
    CHECK_FALSE(from_json(R"({"board":5})", partial));
    CHECK_FALSE(from_json(R"({"addr_type":2})", partial));
    CHECK_FALSE(from_json(R"({"max_app_size":-1})", partial));
}

TEST_CASE("Target config: tuning knobs round-trip and are range-checked", "[target]") {
    TargetConfig t;
    t.id = "attic";
    t.prn = 2;
    t.interval_ms = 20;
    t.max_attempts = 5;
    t.arm_timeout_s = 900;
    t.start_settle_ms = 750;
    t.prn_timeout_ms = 8000;
    t.last_ok_version = "v1.18.1";
    t.last_ok_at = 1793000000;
    CHECK(validate(t).empty());
    TargetConfig back;
    REQUIRE(from_json(to_json(t), back));
    CHECK(back.prn == 2);
    CHECK(back.interval_ms == 20);
    CHECK(back.max_attempts == 5);
    CHECK(back.arm_timeout_s == 900);
    CHECK(back.start_settle_ms == 750);
    CHECK(back.prn_timeout_ms == 8000);
    CHECK(back.last_ok_version == "v1.18.1");
    CHECK(back.last_ok_at == 1793000000);

    // An M3a record without the new keys gets the defaults.
    TargetConfig old;
    REQUIRE(from_json(R"({"id":"x","mac":"F4:1C:36:A7:33:95"})", old));
    CHECK(old.prn == kDefaultPrn);
    CHECK(old.prn == 4);
    CHECK(old.interval_ms == kDefaultIntervalMs);
    CHECK(old.interval_ms == 30);
    CHECK(old.max_attempts == kDefaultMaxAttempts);
    CHECK(old.arm_timeout_s == kDefaultArmTimeoutS);

    TargetConfig r = t;
    r.prn = 9;
    CHECK_FALSE(validate(r).empty());
    r = t;
    r.prn = 0;
    CHECK_FALSE(validate(r).empty());
    r = t;
    r.prn = 8;  // the cap, above the default
    CHECK(validate(r).empty());
    r = t;
    r.interval_ms = 14;  // 11.25 ms and below overran OTAFIX on the bench
    CHECK_FALSE(validate(r).empty());
    r = t;
    r.interval_ms = 101;
    CHECK_FALSE(validate(r).empty());
    r = t;
    r.interval_ms = 15;
    CHECK(validate(r).empty());
    r.interval_ms = 100;
    CHECK(validate(r).empty());
    r = t;
    r.max_attempts = 11;
    CHECK_FALSE(validate(r).empty());
    r = t;
    r.arm_timeout_s = 5;
    CHECK_FALSE(validate(r).empty());
    r = t;
    r.prn_timeout_ms = 100;
    CHECK_FALSE(validate(r).empty());
}

TEST_CASE("Target config: PUT merge", "[target]") {
    TargetConfig t;
    // Create: id from the path, MAC normalised, untouched fields keep defaults.
    CHECK(apply_json(R"({"board":"rak4631","mac":"f4:1c:36:a7:33:95","manifest_url":"http://fw.lan/m.json"})",
                     "attic", t)
              .empty());
    CHECK(t.id == "attic");
    CHECK(t.mac == "F4:1C:36:A7:33:95");
    CHECK(t.prn == kDefaultPrn);

    // Update one field; the rest stay.
    CHECK(apply_json(R"({"prn":8,"interval_ms":20})", "attic", t).empty());
    CHECK(t.prn == 8);
    CHECK(t.interval_ms == 20);
    CHECK(t.board == "rak4631");

    // A GET result can be PUT back: read-only fields are ignored.
    t.last_ok_version = "v1";
    CHECK(apply_json(R"({"id":"attic","last_ok_version":"forged","last_ok_at":5})", "attic", t).empty());
    CHECK(t.last_ok_version == "v1");
    CHECK(t.last_ok_at == 0);

    const TargetConfig before = t;
    CHECK(apply_json(R"({"id":"other"})", "attic", t) == "body id does not match the URL");
    CHECK(apply_json(R"({"bord":"x"})", "attic", t) == "unknown field 'bord'");
    CHECK_FALSE(apply_json(R"({"prn":"8"})", "attic", t).empty());
    CHECK_FALSE(apply_json(R"({"prn":-1})", "attic", t).empty());
    CHECK_FALSE(apply_json(R"({"prn":20})", "attic", t).empty());
    CHECK_FALSE(apply_json(R"({"interval_ms":5})", "attic", t).empty());
    CHECK_FALSE(apply_json(R"({"mac":"nope"})", "attic", t).empty());
    CHECK_FALSE(apply_json(R"([1])", "attic", t).empty());
    CHECK_FALSE(apply_json("{", "attic", t).empty());
    CHECK_FALSE(apply_json("{}", "Bad Id", t).empty());
    CHECK(to_json(t) == to_json(before));  // refused updates change nothing
}

TEST_CASE("Target config: a stored record that breaks the rules can be repaired", "[target]") {
    // e.g. hand-edited NVS, or a rule added after the record was saved.
    TargetConfig t;
    REQUIRE(from_json(R"({"id":"attic","board":"rak4631","prn":20,"interval_ms":500,"max_attempts":0,"mac":"nope",)"
                      R"("manifest_url":"https://x/m.json","last_ok_version":"v1"})",
                      t));
    CHECK_FALSE(validate(t).empty());

    TargetConfig fixed = t;
    CHECK(sanitize(fixed) == 5);
    CHECK(validate(fixed).empty());
    CHECK(fixed.prn == kDefaultPrn);
    CHECK(fixed.interval_ms == kDefaultIntervalMs);
    CHECK(fixed.max_attempts == kDefaultMaxAttempts);
    CHECK(fixed.mac.empty());
    CHECK(fixed.manifest_url.empty());
    CHECK(fixed.board == "rak4631");          // valid fields are kept
    CHECK(fixed.last_ok_version == "v1");
    CHECK(sanitize(fixed) == 0);

    // A PUT touching one field succeeds; the broken ones fall back to defaults.
    CHECK(apply_json(R"({"name":"Attic"})", "attic", t).empty());
    CHECK(t.name == "Attic");
    CHECK(t.prn == kDefaultPrn);
    CHECK(t.board == "rak4631");
}
