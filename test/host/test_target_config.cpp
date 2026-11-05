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
