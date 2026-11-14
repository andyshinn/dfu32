// SPDX-License-Identifier: MIT
//
// The set of stored targets (SPEC.md §9.1): up to CONFIG_DFU32_MAX_TARGETS
// TargetConfig records, one of them the default. Pure logic, host-tested; the
// `config` component loads it from NVS and persists the TargetWrites each
// mutation returns.
//
// Storage model: each target is one JSON record in its own NVS slot
// (`tgt0`..`tgt9`), so saving a target is still a single atomic write, and the
// default is a separate key (`tgt_def`) holding the default target's id. The
// default resolves to the stored id when a target has it, otherwise to the
// first target in slot order, so a power cut between two writes never leaves
// the device without a usable default.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dfu32_core/target_config.hpp"

namespace dfu32 {

/// NVS slots that are ever read (`tgt0`..`tgt9`). The build's cap
/// (CONFIG_DFU32_MAX_TARGETS, 1..kMaxTargetSlots) limits how many targets can
/// be created; loading reads every slot, so lowering the cap in a later build
/// never hides or loses targets that are already stored.
inline constexpr size_t kMaxTargetSlots = 10;

struct StoredTarget {
    uint8_t slot = 0;  // NVS slot: key "tgt<slot>"
    TargetConfig cfg;
    /// The id the API and the console use (cfg.effective_id()).
    std::string id() const { return cfg.effective_id(); }
};

/// What a mutation needs written to NVS, in this order: save_slot, then
/// erase_slot, then default_id, then purge_slots. Empty means "nothing to do".
struct TargetWrites {
    std::optional<uint8_t> save_slot;      // write the table's record in this slot
    std::optional<uint8_t> erase_slot;     // remove this slot's record
    std::optional<std::string> default_id;  // write `tgt_def` ("" = erase the key)
    /// Slots whose stored record was skipped on load (corrupt, or a duplicate
    /// id): erased with the first change, so they can't come back later.
    std::vector<uint8_t> purge_slots;
    bool empty() const { return !save_slot && !erase_slot && !default_id && purge_slots.empty(); }
};

class TargetTable {
public:
    explicit TargetTable(size_t max_targets = 1);

    // ---- loading (config::load_targets) ----

    /// Adds a record read from NVS slot `slot`. Ignores it (returns false) if
    /// the slot is out of range or already loaded, or its effective id is
    /// already taken by an earlier slot.
    bool add_loaded(uint8_t slot, TargetConfig t);
    /// A slot whose record exists but was not loaded (corrupt, or its id is
    /// a duplicate). New targets avoid it, and the next change erases it.
    void add_skipped(uint8_t slot);
    /// The raw `tgt_def` value ("" when the key is missing).
    void set_loaded_default(std::string id);

    // ---- queries ----

    size_t size() const { return targets_.size(); }
    bool empty() const { return targets_.empty(); }
    size_t max_targets() const { return max_; }
    /// No new target can be created (size() >= max_targets()).
    bool full() const { return targets_.size() >= max_; }
    /// All targets in slot order.
    const std::vector<StoredTarget>& targets() const { return targets_; }
    /// By effective id; nullptr if none.
    const StoredTarget* find(std::string_view id) const;
    /// The record stored in `slot`; nullptr if none.
    const StoredTarget* at_slot(uint8_t slot) const;
    /// The default target: the one named by `tgt_def`, else the first in slot
    /// order; nullptr when there are no targets.
    const StoredTarget* default_target() const;
    /// default_target()'s id, or "" when there are none.
    std::string default_id() const;
    bool is_default(std::string_view id) const;

    // ---- mutations ----
    // Each validates first and changes nothing on failure. On success it
    // updates the table and returns the writes that persist the change.

    enum class PutStatus : uint8_t { Created, Updated, Full, Invalid };
    struct PutResult {
        PutStatus status = PutStatus::Invalid;
        std::string error;    // why, for Full / Invalid
        TargetWrites writes;  // for Created / Updated
        uint8_t slot = 0;     // the target's slot, for Created / Updated
    };

    /// PUT /api/v1/targets/{id}: apply_json() onto the stored target with
    /// that id, or onto defaults for a new one (Full when the table is full).
    /// The first target created becomes the default, and so does any target
    /// whose body has "default": true ("default": false changes nothing: make
    /// another target the default instead).
    PutResult put_json(const std::string& id, std::string_view body);

    /// Stores a whole record (the console): updates the target with the same
    /// effective id, or creates it (Full when the table is full). `t` must
    /// pass validate(); its MAC is normalized.
    PutResult put(TargetConfig t);

    /// Deletes a target. Deleting the default makes the first remaining
    /// target (slot order) the default. False if there is no such target.
    bool remove(std::string_view id, TargetWrites& w);

    /// Makes `id` the default. False if there is no such target.
    bool set_default(std::string_view id, TargetWrites& w);

    /// Gives target `from` the id `to` (same slot, same settings); a default
    /// target stays the default. "" on success, otherwise why (no such
    /// target, invalid id, or `to` already taken).
    std::string rename(std::string_view from, const std::string& to, TargetWrites& w);

    /// After a successful flash: records last_ok_version / last_ok_at on
    /// target `id`. False (no writes) if the target no longer exists.
    bool record_success(std::string_view id, const std::string& version, uint64_t at, TargetWrites& w);

private:
    PutResult put_json_record(const std::string& id, std::string_view body);
    PutResult put_record(TargetConfig t);
    StoredTarget* find_mut(std::string_view id);
    std::optional<uint8_t> free_slot() const;
    void insert_sorted(StoredTarget st);
    /// Emits a `tgt_def` write when the stored key differs from what the
    /// default resolves to (after a create, delete or rename).
    void sync_default(TargetWrites& w);
    /// A stored `tgt_def` naming no target (a power cut between a delete or
    /// rename and its default write) is pinned to the default it resolves to
    /// now, before a create or rename could reuse that id and silently make
    /// the new target the default.
    void pin_stale_default(TargetWrites& w);
    /// Moves the skipped slots into `w.purge_slots` (once).
    void purge_skipped(TargetWrites& w);

    size_t max_;
    std::vector<StoredTarget> targets_;  // slot order
    std::string stored_default_;         // raw `tgt_def`
    std::vector<uint8_t> skipped_;       // slots with a record that wasn't loaded
};

}  // namespace dfu32
