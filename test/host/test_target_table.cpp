// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "dfu32_core/target_table.hpp"

using namespace dfu32;

namespace {

TargetConfig make(const std::string& id, const std::string& mac = "") {
    TargetConfig t;
    t.id = id;
    t.mac = mac;
    return t;
}

}  // namespace

TEST_CASE("Target table: empty, cap clamped", "[target_table]") {
    TargetTable t(5);
    CHECK(t.empty());
    CHECK(t.max_targets() == 5);
    CHECK(t.default_target() == nullptr);
    CHECK(t.default_id().empty());
    CHECK(TargetTable(0).max_targets() == 1);
    CHECK(TargetTable(99).max_targets() == kMaxTargetSlots);
}

TEST_CASE("Target table: the first target created becomes the default", "[target_table]") {
    TargetTable t(3);
    auto r = t.put_json("attic", R"({"mac":"c2:4e:11:9a:03:7f"})");
    REQUIRE(r.status == TargetTable::PutStatus::Created);
    CHECK(r.slot == 0);
    CHECK(r.writes.save_slot == uint8_t{0});
    REQUIRE(r.writes.default_id);
    CHECK(*r.writes.default_id == "attic");
    CHECK(t.find("attic")->cfg.mac == "C2:4E:11:9A:03:7F");

    // A second target takes the next slot and leaves the default alone.
    r = t.put_json("garage", R"({"mac":"C2:4E:11:9A:03:80"})");
    REQUIRE(r.status == TargetTable::PutStatus::Created);
    CHECK(r.slot == 1);
    CHECK_FALSE(r.writes.default_id);
    CHECK(t.default_id() == "attic");

    // An update is one write: the record, nothing else.
    r = t.put_json("garage", R"({"board":"rak4631"})");
    REQUIRE(r.status == TargetTable::PutStatus::Updated);
    CHECK(r.writes.save_slot == uint8_t{1});
    CHECK_FALSE(r.writes.erase_slot);
    CHECK_FALSE(r.writes.default_id);
    CHECK(t.find("garage")->cfg.board == "rak4631");
    CHECK(t.find("garage")->cfg.mac == "C2:4E:11:9A:03:80");  // merged, not replaced
}

TEST_CASE("Target table: the cap refuses new targets but not updates", "[target_table]") {
    TargetTable t(2);
    REQUIRE(t.put_json("a", "{}").status == TargetTable::PutStatus::Created);
    REQUIRE(t.put_json("b", "{}").status == TargetTable::PutStatus::Created);
    CHECK(t.full());
    const auto r = t.put_json("c", "{}");
    CHECK(r.status == TargetTable::PutStatus::Full);
    CHECK(r.writes.empty());
    CHECK(r.error.find("at most 2 targets") != std::string::npos);
    CHECK(t.size() == 2);
    CHECK(t.put_json("b", R"({"name":"B"})").status == TargetTable::PutStatus::Updated);
    CHECK(t.put(make("c")).status == TargetTable::PutStatus::Full);
}

TEST_CASE("Target table: a bad body changes nothing", "[target_table]") {
    TargetTable t(3);
    REQUIRE(t.put_json("a", R"({"mac":"C2:4E:11:9A:03:7F"})").status == TargetTable::PutStatus::Created);
    auto r = t.put_json("a", R"({"mac":"nope"})");
    CHECK(r.status == TargetTable::PutStatus::Invalid);
    CHECK_FALSE(r.error.empty());
    CHECK(r.writes.empty());
    CHECK(t.find("a")->cfg.mac == "C2:4E:11:9A:03:7F");
    r = t.put_json("b", R"({"bogus":1})");
    CHECK(r.status == TargetTable::PutStatus::Invalid);
    CHECK(t.find("b") == nullptr);
    // Invalid is reported before Full, so a typo isn't misread as "delete one first".
    TargetTable one(1);
    REQUIRE(one.put_json("a", "{}").status == TargetTable::PutStatus::Created);
    CHECK(one.put_json("b", R"({"bogus":1})").status == TargetTable::PutStatus::Invalid);
}

TEST_CASE("Target table: deleting the default promotes the first remaining", "[target_table]") {
    TargetTable t(5);
    t.put_json("a", "{}");
    t.put_json("b", "{}");
    t.put_json("c", "{}");
    TargetWrites w;
    REQUIRE(t.set_default("b", w));
    REQUIRE(w.default_id);
    CHECK(*w.default_id == "b");

    // Making it the default again writes nothing.
    w = {};
    REQUIRE(t.set_default("b", w));
    CHECK(w.empty());

    // Deleting a non-default target leaves the default key alone.
    w = {};
    REQUIRE(t.remove("c", w));
    CHECK(w.erase_slot == uint8_t{2});
    CHECK_FALSE(w.default_id);
    CHECK(t.default_id() == "b");

    w = {};
    REQUIRE(t.remove("b", w));
    CHECK(w.erase_slot == uint8_t{1});
    REQUIRE(w.default_id);
    CHECK(*w.default_id == "a");
    CHECK(t.is_default("a"));

    // The last one: the default key is erased.
    w = {};
    REQUIRE(t.remove("a", w));
    REQUIRE(w.default_id);
    CHECK(w.default_id->empty());
    CHECK(t.default_target() == nullptr);

    w = {};
    CHECK_FALSE(t.remove("a", w));
    CHECK_FALSE(t.set_default("a", w));
    CHECK(w.empty());
}

TEST_CASE("Target table: freed slots are reused and the default sticks", "[target_table]") {
    TargetTable t(3);
    t.put_json("a", "{}");
    t.put_json("b", "{}");
    TargetWrites w;
    t.set_default("b", w);
    w = {};
    t.remove("a", w);
    CHECK_FALSE(w.default_id);
    // "z" lands in slot 0, ahead of "b" in slot order, but "b" stays the default.
    const auto r = t.put_json("z", "{}");
    CHECK(r.slot == 0);
    CHECK_FALSE(r.writes.default_id);
    CHECK(t.targets().front().id() == "z");
    CHECK(t.default_id() == "b");
}

TEST_CASE("Target table: loading", "[target_table]") {
    TargetTable t(2);
    CHECK(t.add_loaded(3, make("c")));
    CHECK(t.add_loaded(0, make("a")));
    CHECK_FALSE(t.add_loaded(0, make("x")));                // slot taken
    CHECK_FALSE(t.add_loaded(1, make("a")));                // id taken
    CHECK_FALSE(t.add_loaded(kMaxTargetSlots, make("y")));  // no such slot
    CHECK(t.size() == 2);
    CHECK(t.targets()[0].id() == "a");  // slot order
    CHECK(t.targets()[1].id() == "c");

    // No stored default: the first in slot order.
    CHECK(t.default_id() == "a");
    // A stored default that names a target wins.
    t.set_loaded_default("c");
    CHECK(t.default_id() == "c");
    // A stale one (the target was deleted mid-update) falls back.
    t.set_loaded_default("gone");
    CHECK(t.default_id() == "a");

    // Stored by a build with a higher cap: all kept, none can be added, and
    // the next create (after a delete) finds a free slot below the old ones.
    CHECK(t.full());
    TargetWrites w;
    REQUIRE(t.remove("a", w));
    // The stale stored default is repaired on the next default change.
    REQUIRE(w.default_id);
    CHECK(*w.default_id == "c");
    CHECK(t.full() == false);
    CHECK(t.put(make("b")).slot == 0);
}

TEST_CASE("Target table: an id-less (single-target build) record is 'target'", "[target_table]") {
    TargetTable t(3);
    REQUIRE(t.add_loaded(0, make("", "C2:4E:11:9A:03:7F")));
    CHECK(t.find("target") != nullptr);
    CHECK(t.default_id() == "target");
    CHECK_FALSE(t.add_loaded(1, make("target")));  // the same id

    // PUT to its effective id updates it, and stores the id from then on.
    auto r = t.put_json("target", R"({"board":"rak4631"})");
    REQUIRE(r.status == TargetTable::PutStatus::Updated);
    CHECK(t.find("target")->cfg.id == "target");
    // PUT to another id creates a second target (no "adoption" any more).
    r = t.put_json("attic", "{}");
    CHECK(r.status == TargetTable::PutStatus::Created);
    CHECK(t.size() == 2);
}

TEST_CASE("Target table: put() from the console", "[target_table]") {
    TargetTable t(3);
    auto c = make("", "c2:4e:11:9a:03:7f");
    auto r = t.put(c);
    REQUIRE(r.status == TargetTable::PutStatus::Created);
    CHECK(t.find("target")->cfg.id == "target");
    CHECK(t.find("target")->cfg.mac == "C2:4E:11:9A:03:7F");
    REQUIRE(r.writes.default_id);

    c = t.find("target")->cfg;
    c.board = "rak4631";
    r = t.put(c);
    CHECK(r.status == TargetTable::PutStatus::Updated);
    CHECK(t.size() == 1);

    c.max_app_size = 78;  // invalid
    r = t.put(c);
    CHECK(r.status == TargetTable::PutStatus::Invalid);
    CHECK(t.find("target")->cfg.max_app_size == 0);
}

TEST_CASE("Target table: rename keeps slot, settings and default", "[target_table]") {
    TargetTable t(3);
    t.put_json("target", R"({"mac":"C2:4E:11:9A:03:7F"})");
    t.put_json("b", "{}");
    TargetWrites w;
    CHECK(t.rename("target", "attic", w).empty());
    CHECK(w.save_slot == uint8_t{0});
    REQUIRE(w.default_id);
    CHECK(*w.default_id == "attic");
    CHECK(t.find("target") == nullptr);
    CHECK(t.find("attic")->cfg.mac == "C2:4E:11:9A:03:7F");
    CHECK(t.default_id() == "attic");

    // Renaming a non-default target doesn't touch the default.
    w = {};
    CHECK(t.rename("b", "garage", w).empty());
    CHECK_FALSE(w.default_id);
    CHECK(t.default_id() == "attic");

    w = {};
    CHECK_FALSE(t.rename("garage", "attic", w).empty());  // taken
    CHECK_FALSE(t.rename("garage", "Bad Id", w).empty());
    CHECK_FALSE(t.rename("garage", "", w).empty());
    CHECK_FALSE(t.rename("nope", "x", w).empty());
    CHECK(w.empty());
    CHECK(t.rename("garage", "garage", w).empty());  // no-op
    CHECK(w.empty());
}

TEST_CASE("Target table: record_success", "[target_table]") {
    TargetTable t(3);
    t.put_json("a", "{}");
    t.put_json("b", "{}");
    TargetWrites w;
    REQUIRE(t.record_success("b", "v1.18.0", 1763000000, w));
    CHECK(w.save_slot == uint8_t{1});
    CHECK(t.find("b")->cfg.last_ok_version == "v1.18.0");
    CHECK(t.find("b")->cfg.last_ok_at == 1763000000);
    CHECK(t.find("a")->cfg.last_ok_version.empty());
    w = {};
    REQUIRE(t.record_success("b", std::string(100, 'v'), 1, w));
    CHECK(t.find("b")->cfg.last_ok_version.size() == 63);
    w = {};
    CHECK_FALSE(t.record_success("gone", "v", 1, w));
    CHECK(w.empty());
}

TEST_CASE("Target table: worst-case records fit the NVS budget", "[target_table]") {
    // SPEC.md §9.1: each record is one NVS string; at most 4000 bytes, and a
    // 4 MB board's 24 KB NVS must hold kMaxTargetSlots of them comfortably.
    TargetConfig w;
    w.id = std::string(31, 'a');
    w.name = std::string(63, 'n');
    w.board = std::string(31, 'b');
    w.mac = "C2:4E:11:9A:03:7F";
    w.manifest_url = "http://" + std::string(248, 'u');
    w.last_ok_version = std::string(63, 'v');
    w.max_app_size = 16u * 1024 * 1024;
    w.prn_timeout_ms = 60000;
    w.arm_timeout_s = 3600;
    w.last_ok_at = UINT64_C(0xFFFFFFFFFF);
    REQUIRE(validate(w).empty());
    const size_t len = to_json(w).size() + 1;  // with the NUL NVS stores
    CHECK(len < 800);
    // NVS entries are 32 bytes; a string takes one header entry plus its data.
    const size_t entries = 1 + (len + 31) / 32;
    CHECK(entries * kMaxTargetSlots < 126 * 2);  // under two 4 KB pages for all ten
}

TEST_CASE("Target table: \"default\" in a PUT body", "[target_table]") {
    TargetTable t(3);
    t.put_json("a", "{}");
    auto r = t.put_json("b", R"({"default":true})");
    REQUIRE(r.status == TargetTable::PutStatus::Created);
    REQUIRE(r.writes.default_id);
    CHECK(*r.writes.default_id == "b");
    CHECK(t.default_id() == "b");

    // false is accepted (a GET result sent back) and changes nothing.
    r = t.put_json("b", R"({"default":false,"name":"B"})");
    REQUIRE(r.status == TargetTable::PutStatus::Updated);
    CHECK_FALSE(r.writes.default_id);
    CHECK(t.default_id() == "b");
    r = t.put_json("a", R"({"default":true})");
    REQUIRE(r.status == TargetTable::PutStatus::Updated);
    CHECK(r.writes.save_slot == uint8_t{0});
    REQUIRE(r.writes.default_id);
    CHECK(t.default_id() == "a");

    // It must be a boolean, and it isn't stored in the record.
    r = t.put_json("a", R"({"default":"yes"})");
    CHECK(r.status == TargetTable::PutStatus::Invalid);
    CHECK(to_json(t.find("a")->cfg).find("\"default\"") == std::string::npos);

    // A GET /targets/{id} answer (with "default" and "last_run") can be sent back.
    r = t.put_json("a", R"({"id":"a","default":true,"last_run":{"seq":3},"last_ok_version":"x"})");
    CHECK(r.status == TargetTable::PutStatus::Updated);
    CHECK(t.find("a")->cfg.last_ok_version.empty());  // read-only
}

TEST_CASE("Target table: a stale stored default isn't adopted by a new target", "[target_table]") {
    // Power cut after a delete (or rename) wrote the record but not tgt_def.
    TargetTable t(3);
    REQUIRE(t.add_loaded(1, make("a")));
    t.set_loaded_default("b");
    CHECK(t.default_id() == "a");
    // "b" lands in slot 0, ahead of "a", and its id matches the stale key.
    auto r = t.put_json("b", "{}");
    REQUIRE(r.status == TargetTable::PutStatus::Created);
    CHECK(r.slot == 0);
    CHECK(t.default_id() == "a");
    REQUIRE(r.writes.default_id);
    CHECK(*r.writes.default_id == "a");  // the key is repaired

    // The same through rename(), and through put().
    TargetTable u(3);
    REQUIRE(u.add_loaded(0, make("a")));
    REQUIRE(u.add_loaded(1, make("x")));
    u.set_loaded_default("b");
    TargetWrites w;
    CHECK(u.rename("x", "b", w).empty());
    CHECK(u.default_id() == "a");
    REQUIRE(w.default_id);
    CHECK(*w.default_id == "a");

    TargetTable v(3);
    REQUIRE(v.add_loaded(2, make("a")));
    v.set_loaded_default("b");
    r = v.put(make("b"));
    REQUIRE(r.status == TargetTable::PutStatus::Created);
    CHECK(v.default_id() == "a");

    // Stale with no targets at all: the first target created is the default.
    TargetTable e(3);
    e.set_loaded_default("b");
    r = e.put_json("b", "{}");
    CHECK(e.default_id() == "b");
    REQUIRE(r.writes.default_id);
    CHECK(*r.writes.default_id == "b");
}

TEST_CASE("Target table: skipped slots are avoided and purged", "[target_table]") {
    // Slot 0 is an id-less record ("target"); slot 1 holds a duplicate "target"
    // and slot 2 a corrupt record. Neither may come back or be reused silently.
    TargetTable t(5);
    REQUIRE(t.add_loaded(0, make("")));
    CHECK_FALSE(t.add_loaded(1, make("target")));
    t.add_skipped(1);
    t.add_skipped(2);
    t.add_skipped(0);  // loaded: ignored

    // A new target takes a real gap, not a skipped slot.
    auto r = t.put_json("roof", "{}");
    REQUIRE(r.status == TargetTable::PutStatus::Created);
    CHECK(r.slot == 3);
    CHECK(r.writes.purge_slots == std::vector<uint8_t>{1, 2});
    // Purged once.
    r = t.put_json("roof", R"({"name":"Roof"})");
    CHECK(r.writes.purge_slots.empty());

    // Deleting also purges, so the duplicate can't reappear after a reload.
    TargetTable d(5);
    REQUIRE(d.add_loaded(0, make("")));
    d.add_skipped(3);
    TargetWrites w;
    REQUIRE(d.remove("target", w));
    CHECK(w.erase_slot == uint8_t{0});
    CHECK(w.purge_slots == std::vector<uint8_t>{3});

    // When only skipped slots are free, one is reused (its write replaces the
    // bad record) and it isn't also purged.
    TargetTable f(kMaxTargetSlots);
    for (size_t s = 0; s + 1 < kMaxTargetSlots; ++s) {
        REQUIRE(f.add_loaded(static_cast<uint8_t>(s), make("t" + std::to_string(s))));
    }
    f.add_skipped(static_cast<uint8_t>(kMaxTargetSlots - 1));
    r = f.put_json("last", "{}");
    REQUIRE(r.status == TargetTable::PutStatus::Created);
    CHECK(r.slot == kMaxTargetSlots - 1);
    CHECK(r.writes.purge_slots.empty());
}
