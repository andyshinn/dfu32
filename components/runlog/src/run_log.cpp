// SPDX-License-Identifier: MIT
#include "runlog/run_log.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

#include "dfu_proto/staging_format.hpp"  // crc32_ieee

namespace runlog {
namespace {

constexpr char kMagic[8] = {'D', 'F', '3', '2', 'R', 'U', 'N', '1'};

void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

using SlotBuf = std::array<uint8_t, kSlotSize>;

/// A slot is 4 KB: keep it off the caller's stack. RunLog runs on small
/// stacks (app_main's 3.5 KB when recording an interrupted run at boot, the
/// HTTP server's 8 KB), and a stack copy there overflowed. Null when out of
/// memory.
std::unique_ptr<SlotBuf> slot_buf() { return std::unique_ptr<SlotBuf>(new (std::nothrow) SlotBuf{}); }

uint32_t payload_crc(std::string_view json, std::string_view log) {
    uint32_t crc = dfu::crc32_ieee({reinterpret_cast<const uint8_t*>(json.data()), json.size()});
    return dfu::crc32_ieee({reinterpret_cast<const uint8_t*>(log.data()), log.size()}, crc);
}

bool header_seq(std::span<const uint8_t> in, uint32_t& seq) {
    if (in.size() < kSlotHeaderSize || std::memcmp(in.data(), kMagic, sizeof(kMagic)) != 0) return false;
    seq = get_u32(in.data() + 8);
    return seq != 0 && seq != 0xFFFFFFFFu;
}

}  // namespace

bool encode_slot(uint32_t seq, std::string_view json, std::string_view log, std::span<uint8_t, kSlotSize> out) {
    if (seq == 0 || seq == 0xFFFFFFFFu || json.size() > kSlotPayloadMax) return false;
    const size_t room = kSlotPayloadMax - json.size();
    if (log.size() > room) {
        log.remove_prefix(log.size() - room);
        const size_t nl = log.find('\n');
        log.remove_prefix(nl == std::string_view::npos ? log.size() : nl + 1);
    }
    std::fill(out.begin(), out.end(), uint8_t{0xFF});
    uint8_t* p = out.data();
    std::memcpy(p, kMagic, sizeof(kMagic));
    put_u32(p + 8, seq);
    put_u16(p + 12, static_cast<uint16_t>(json.size()));
    put_u16(p + 14, static_cast<uint16_t>(log.size()));
    put_u32(p + 16, payload_crc(json, log));
    std::memcpy(p + kSlotHeaderSize, json.data(), json.size());
    std::memcpy(p + kSlotHeaderSize + json.size(), log.data(), log.size());
    return true;
}

bool decode_slot(std::span<const uint8_t> in, RunSlot& out) {
    uint32_t seq = 0;
    if (in.size() < kSlotSize || !header_seq(in, seq)) return false;
    const uint8_t* p = in.data();
    const size_t json_len = get_u16(p + 12);
    const size_t log_len = get_u16(p + 14);
    if (json_len + log_len > kSlotPayloadMax) return false;
    const std::string_view json(reinterpret_cast<const char*>(p + kSlotHeaderSize), json_len);
    const std::string_view log(reinterpret_cast<const char*>(p + kSlotHeaderSize + json_len), log_len);
    if (payload_crc(json, log) != get_u32(p + 16)) return false;
    out.seq = seq;
    out.json.assign(json);
    out.log.assign(log);
    return true;
}

void RunLog::scan() {
    any_ = false;
    newest_seq_ = 0;
    newest_index_ = 0;
    const auto buf = slot_buf();
    if (!buf) return;
    for (size_t i = 0; i < storage_.slot_count(); ++i) {
        RunSlot slot;
        if (!storage_.read_slot(i, *buf) || !decode_slot(*buf, slot)) continue;
        if (!any_ || slot.seq > newest_seq_) {
            any_ = true;
            newest_seq_ = slot.seq;
            newest_index_ = i;
        }
    }
}

uint32_t RunLog::append(std::string_view json, std::string_view log) {
    const size_t n = storage_.slot_count();
    if (n == 0) return 0;
    const uint32_t seq = next_seq();
    const size_t index = any_ ? (newest_index_ + 1) % n : 0;
    const auto buf = slot_buf();
    if (!buf || !encode_slot(seq, json, log, *buf) || !storage_.write_slot(index, *buf)) return 0;
    any_ = true;
    newest_seq_ = seq;
    newest_index_ = index;
    return seq;
}

std::vector<RunSlot> RunLog::list(bool with_log) {
    std::vector<RunSlot> out;
    const auto buf = slot_buf();
    if (!buf) return out;
    for (size_t i = 0; i < storage_.slot_count(); ++i) {
        RunSlot slot;
        if (!storage_.read_slot(i, *buf) || !decode_slot(*buf, slot)) continue;
        if (!with_log) slot.log.clear();
        out.push_back(std::move(slot));
    }
    std::sort(out.begin(), out.end(), [](const RunSlot& a, const RunSlot& b) { return a.seq > b.seq; });
    return out;
}

bool RunLog::get(uint32_t seq, RunSlot& out) {
    const auto buf = slot_buf();
    if (!buf) return false;
    for (size_t i = 0; i < storage_.slot_count(); ++i) {
        RunSlot slot;
        if (storage_.read_slot(i, *buf) && decode_slot(*buf, slot) && slot.seq == seq) {
            out = std::move(slot);
            return true;
        }
    }
    return false;
}

}  // namespace runlog
