// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "dfu32_core/json.hpp"

#include <array>
#include <string>
#include <vector>

#include "runlog/log_ring.hpp"
#include "runlog/run_log.hpp"
#include "runlog/run_record.hpp"

using runlog::LogRing;
using runlog::RunLog;
using runlog::RunSlot;

namespace {

class MemStorage : public runlog::ISlotStorage {
public:
    explicit MemStorage(size_t n) : slots(n) {
        for (auto& s : slots) s.fill(0xFF);
    }
    size_t slot_count() const override { return slots.size(); }
    bool read_slot(size_t i, std::span<uint8_t, runlog::kSlotSize> out) override {
        std::copy(slots[i].begin(), slots[i].end(), out.begin());
        return true;
    }
    bool write_slot(size_t i, std::span<const uint8_t, runlog::kSlotSize> d) override {
        std::copy(d.begin(), d.end(), slots[i].begin());
        ++writes;
        return true;
    }
    std::vector<std::array<uint8_t, runlog::kSlotSize>> slots;
    int writes = 0;
};

}  // namespace

// ---- LogRing -------------------------------------------------------------------

TEST_CASE("Log ring keeps everything while it fits", "[logring]") {
    std::array<char, 64> buf{};
    LogRing ring(buf);
    ring.append("a\n");
    const uint64_t mark = ring.position();
    ring.append("bb\n");
    ring.append("ccc\n");
    CHECK(ring.tail(1000) == "a\nbb\nccc\n");
    CHECK(ring.since(mark, 1000) == "bb\nccc\n");
    CHECK(ring.position() == 9);
}

TEST_CASE("Log ring wraps and never starts mid-line when cut", "[logring]") {
    std::array<char, 16> buf{};
    LogRing ring(buf);
    ring.append("line-one\n");     // 9
    ring.append("line-two\n");     // 18 -> wrapped
    ring.append("line-three\n");   // 29
    CHECK(ring.oldest() == 13);
    // 16 retained bytes start mid-"line-two"; output starts at the next line.
    CHECK(ring.tail(1000) == "line-three\n");
    CHECK(ring.since(0, 1000) == "line-three\n");
}

TEST_CASE("Log ring limits by max bytes, aligned to a line", "[logring]") {
    std::array<char, 256> buf{};
    LogRing ring(buf);
    for (int i = 0; i < 10; ++i) ring.append("0123456789\n");  // 11 bytes each
    // Last 25 bytes start mid-line; output starts at the next line boundary.
    CHECK(ring.tail(25) == "0123456789\n0123456789\n");
    // Exactly two whole lines: nothing may be dropped (review finding).
    CHECK(ring.tail(22) == "0123456789\n0123456789\n");
    CHECK(ring.tail(11) == "0123456789\n");
}

TEST_CASE("Log ring handles a write larger than the buffer", "[logring]") {
    std::array<char, 8> buf{};
    LogRing ring(buf);
    ring.append("abcdefghijklmnop\n");
    CHECK(ring.position() == 17);
    CHECK(ring.since(17, 100).empty());
    CHECK(ring.tail(100).empty());  // retained bytes contain no line start
}

// ---- slot codec ----------------------------------------------------------------

TEST_CASE("Slot round trip", "[runlog]") {
    std::array<uint8_t, runlog::kSlotSize> buf{};
    REQUIRE(runlog::encode_slot(7, "{\"a\":1}", "log line\n", buf));
    RunSlot s;
    REQUIRE(runlog::decode_slot(buf, s));
    CHECK(s.seq == 7);
    CHECK(s.json == "{\"a\":1}");
    CHECK(s.log == "log line\n");
}

TEST_CASE("Slot trims the log from the front to fit", "[runlog]") {
    std::string log;
    for (int i = 0; i < 400; ++i) log += "line " + std::to_string(i) + "\n";  // ~3.6 KB
    log += std::string(1000, 'x') + "\nlast line\n";
    std::array<uint8_t, runlog::kSlotSize> buf{};
    REQUIRE(runlog::encode_slot(1, "{}", log, buf));
    RunSlot s;
    REQUIRE(runlog::decode_slot(buf, s));
    CHECK(s.log.size() <= runlog::kSlotPayloadMax - 2);
    CHECK(s.log.ends_with("last line\n"));
    CHECK((s.log.starts_with("line ") || s.log.starts_with("x")));  // starts at a line boundary
}

TEST_CASE("Slot rejects bad input and detects corruption", "[runlog]") {
    std::array<uint8_t, runlog::kSlotSize> buf{};
    CHECK_FALSE(runlog::encode_slot(0, "{}", "", buf));
    CHECK_FALSE(runlog::encode_slot(1, std::string(runlog::kSlotPayloadMax + 1, 'j'), "", buf));

    REQUIRE(runlog::encode_slot(3, "{\"x\":2}", "abc\n", buf));
    buf[runlog::kSlotHeaderSize + 2] ^= 0x01;
    RunSlot s;
    CHECK_FALSE(runlog::decode_slot(buf, s));

    buf.fill(0xFF);
    CHECK_FALSE(runlog::decode_slot(buf, s));  // erased
}

// ---- ring rotation ---------------------------------------------------------------

TEST_CASE("Run log appends, rotates and survives a rescan", "[runlog]") {
    MemStorage mem(3);
    RunLog log(mem);
    log.scan();
    CHECK(log.next_seq() == 1);
    CHECK(log.append("{\"r\":1}", "") == 1);
    CHECK(log.append("{\"r\":2}", "") == 2);
    CHECK(log.append("{\"r\":3}", "") == 3);
    CHECK(log.append("{\"r\":4}", "") == 4);  // overwrites the oldest (seq 1)

    auto all = log.list();
    REQUIRE(all.size() == 3);
    CHECK(all[0].seq == 4);
    CHECK(all[2].seq == 2);

    RunLog again(mem);  // e.g. after a reboot
    again.scan();
    CHECK(again.next_seq() == 5);
    CHECK(again.append("{\"r\":5}", "") == 5);
    RunSlot s;
    CHECK_FALSE(again.get(2, s));  // rotated out
    REQUIRE(again.get(5, s));
    CHECK(s.json == "{\"r\":5}");
}

TEST_CASE("Run log skips a corrupted slot", "[runlog]") {
    MemStorage mem(4);
    RunLog log(mem);
    log.scan();
    log.append("{}", "");
    log.append("{}", "");
    mem.slots[1][runlog::kSlotHeaderSize] ^= 0xFF;  // corrupt seq 2
    RunLog again(mem);
    again.scan();
    CHECK(again.next_seq() == 2);  // newest valid is 1
    CHECK(again.list().size() == 1);
}

// ---- JSON --------------------------------------------------------------------

TEST_CASE("Run record JSON escapes and reads back", "[runlog]") {
    runlog::RunRecord r;
    r.seq = 12;
    r.result = "FAILED";
    r.error = "LINK_LOST in \"STREAM\"\n(status 0x00)";
    r.target = "F4:1C:36:A7:33:95";
    r.image_size = 413680;
    r.target_erased = "yes";
    r.wifi_off = true;
    r.attempts.push_back({1, 8, 8, "LINK_LOST", "LINK_OR_OVERRUN", 27328});
    r.attempts.push_back({2, 30, 8, "OK", "NONE", 413680});
    const std::string j = runlog::to_json(r);
    CHECK(j.front() == '{');
    CHECK(j.back() == '}');
    CHECK(runlog::json_get_uint(j, "seq") == 12);
    CHECK(runlog::json_get_string(j, "result") == "FAILED");
    CHECK(runlog::json_get_string(j, "error") == r.error);
    CHECK(runlog::json_get_uint(j, "image_size") == 413680);
    CHECK(runlog::json_get_string(j, "missing").empty());
    CHECK(runlog::json_get_uint(j, "missing", 99) == 99);
    CHECK(j.find("\"attempts\":[{\"n\":1,") != std::string::npos);
    CHECK(j.find("\"wifi_off\":true") != std::string::npos);
    // The whole record is valid JSON (the M3b API serves it as-is).
    dfu32::json::Doc doc;
    REQUIRE(doc.parse(j));
    CHECK(doc.root().get("error").str() == r.error);
    CHECK(doc.root().get("attempts").size() == 2);
    CHECK(runlog::json_escape(std::string(1, '\x01')) == "\\u0001");
    CHECK(runlog::json_get_string("{\"k\":\"a\\u0001b\"}", "k") == std::string("a\x01" "b"));
}
