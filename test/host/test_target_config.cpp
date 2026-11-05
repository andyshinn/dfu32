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
