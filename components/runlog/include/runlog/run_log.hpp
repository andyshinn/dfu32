// SPDX-License-Identifier: MIT
//
// Persisted run records (SPEC.md §13.2): a ring of 4 KiB slots in the raw
// `runlog` partition, one slot per flash run. Each slot holds a JSON summary
// plus the tail of that run's log. Pure logic over an ISlotStorage, so the
// format and ring rotation are host-tested; the partition-backed storage lives
// in the ESP-IDF glue.
//
// Slot layout (little-endian):
//   0x00  char[8]  magic "DF32RUN1"
//   0x08  u32      seq (1, 2, 3, ...; 0 and 0xFFFFFFFF are never used)
//   0x0C  u16      json_len
//   0x0E  u16      log_len
//   0x10  u32      crc32 (IEEE) over json + log
//   0x14  json bytes, then log bytes; rest of the slot is 0xFF
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace runlog {

inline constexpr size_t kSlotSize = 4096;
inline constexpr size_t kSlotHeaderSize = 0x14;
inline constexpr size_t kSlotPayloadMax = kSlotSize - kSlotHeaderSize;

struct RunSlot {
    uint32_t seq = 0;
    std::string json;
    std::string log;
};

/// Encodes a slot. The log is trimmed from the front (to a line start) so that
/// json + log fit. Fails only if the JSON alone does not fit or seq is invalid.
bool encode_slot(uint32_t seq, std::string_view json, std::string_view log, std::span<uint8_t, kSlotSize> out);

/// Decodes a slot; false for erased, foreign or corrupted slots.
bool decode_slot(std::span<const uint8_t> in, RunSlot& out);

class ISlotStorage {
public:
    virtual ~ISlotStorage() = default;
    virtual size_t slot_count() const = 0;
    virtual bool read_slot(size_t index, std::span<uint8_t, kSlotSize> out) = 0;
    /// Erases the slot and writes it in full.
    virtual bool write_slot(size_t index, std::span<const uint8_t, kSlotSize> data) = 0;
};

class RunLog {
public:
    explicit RunLog(ISlotStorage& storage) : storage_(storage) {}

    /// Reads every slot header to find the newest record. Call once at boot.
    void scan();

    /// Sequence number the next append will get.
    uint32_t next_seq() const { return newest_seq_ + 1; }

    /// Writes a record into the slot after the newest one (overwriting the
    /// oldest once the ring is full). Returns the seq written, 0 on failure.
    uint32_t append(std::string_view json, std::string_view log);

    /// All valid records, newest first. Logs are dropped unless `with_log`.
    std::vector<RunSlot> list(bool with_log = false);

    bool get(uint32_t seq, RunSlot& out);

private:
    ISlotStorage& storage_;
    uint32_t newest_seq_ = 0;
    size_t newest_index_ = 0;
    bool any_ = false;
};

}  // namespace runlog
