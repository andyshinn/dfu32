// SPDX-License-Identifier: MIT
#include "dfu32_core/target_table.hpp"

#include <algorithm>
#include <utility>

#include "dfu32_core/json.hpp"
#include "dfu32_core/manifest.hpp"  // kMaxVersionLen

namespace dfu32 {

TargetTable::TargetTable(size_t max_targets)
    : max_(std::clamp<size_t>(max_targets, 1, kMaxTargetSlots)) {}

bool TargetTable::add_loaded(uint8_t slot, TargetConfig t) {
    if (slot >= kMaxTargetSlots || at_slot(slot) != nullptr) return false;
    if (find(t.effective_id()) != nullptr) return false;
    insert_sorted(StoredTarget{slot, std::move(t)});
    return true;
}

void TargetTable::add_skipped(uint8_t slot) {
    if (slot < kMaxTargetSlots && at_slot(slot) == nullptr &&
        std::find(skipped_.begin(), skipped_.end(), slot) == skipped_.end()) {
        skipped_.push_back(slot);
    }
}

void TargetTable::set_loaded_default(std::string id) { stored_default_ = std::move(id); }

const StoredTarget* TargetTable::find(std::string_view id) const {
    for (const auto& st : targets_) {
        if (st.id() == id) return &st;
    }
    return nullptr;
}

StoredTarget* TargetTable::find_mut(std::string_view id) {
    for (auto& st : targets_) {
        if (st.id() == id) return &st;
    }
    return nullptr;
}

const StoredTarget* TargetTable::at_slot(uint8_t slot) const {
    for (const auto& st : targets_) {
        if (st.slot == slot) return &st;
    }
    return nullptr;
}

const StoredTarget* TargetTable::default_target() const {
    if (targets_.empty()) return nullptr;
    if (!stored_default_.empty()) {
        if (const StoredTarget* st = find(stored_default_)) return st;
    }
    return &targets_.front();
}

std::string TargetTable::default_id() const {
    const StoredTarget* st = default_target();
    return st != nullptr ? st->id() : std::string{};
}

bool TargetTable::is_default(std::string_view id) const {
    const StoredTarget* st = default_target();
    return st != nullptr && st->id() == id;
}

std::optional<uint8_t> TargetTable::free_slot() const {
    // Any slot NVS can hold, not just those below max_: a table loaded from a
    // build with a larger cap may already use higher slots. size() < max_ is
    // what enforces the cap.
    // A skipped slot still holds a key until it is purged; prefer real gaps.
    std::optional<uint8_t> skipped;
    for (size_t s = 0; s < kMaxTargetSlots; ++s) {
        const auto slot = static_cast<uint8_t>(s);
        if (at_slot(slot) != nullptr) continue;
        if (std::find(skipped_.begin(), skipped_.end(), slot) == skipped_.end()) return slot;
        if (!skipped) skipped = slot;
    }
    return skipped;
}

void TargetTable::insert_sorted(StoredTarget st) {
    const auto pos = std::find_if(targets_.begin(), targets_.end(),
                                  [&](const StoredTarget& o) { return o.slot > st.slot; });
    targets_.insert(pos, std::move(st));
}

void TargetTable::sync_default(TargetWrites& w) {
    const std::string resolved = default_id();
    if (resolved != stored_default_) {
        w.default_id = resolved;
        stored_default_ = resolved;
    }
}

void TargetTable::pin_stale_default(TargetWrites& w) {
    if (stored_default_.empty() || find(stored_default_) != nullptr) return;
    stored_default_ = default_id();
    w.default_id = stored_default_;
}

void TargetTable::purge_skipped(TargetWrites& w) {
    for (const uint8_t slot : skipped_) {
        // A create may have just reused it (only when no other slot was free).
        if (at_slot(slot) == nullptr) w.purge_slots.push_back(slot);
    }
    skipped_.clear();
}

namespace {

std::string full_reason(size_t max) {
    return "this DFU32 holds at most " + std::to_string(max) + " target" + (max == 1 ? "" : "s") +
           ": delete one first";
}

}  // namespace

TargetTable::PutResult TargetTable::put_json(const std::string& id, std::string_view body) {
    PutResult r = put_json_record(id, body);
    if (r.status == PutStatus::Created || r.status == PutStatus::Updated) {
        // apply_json() accepted the body, so "default", if present, is a boolean.
        json::Doc doc;
        if (doc.parse(body) && doc.root().get("default").boolean(false)) set_default(id, r.writes);
        purge_skipped(r.writes);
    }
    return r;
}

TargetTable::PutResult TargetTable::put_json_record(const std::string& id, std::string_view body) {
    PutResult r;
    if (StoredTarget* st = find_mut(id)) {
        TargetConfig next = st->cfg;
        if (auto e = apply_json(body, id, next); !e.empty()) {
            r.error = std::move(e);
            return r;
        }
        st->cfg = std::move(next);
        r.status = PutStatus::Updated;
        r.slot = st->slot;
        r.writes.save_slot = st->slot;
        return r;
    }
    TargetConfig t;
    if (auto e = apply_json(body, id, t); !e.empty()) {
        r.error = std::move(e);
        return r;
    }
    const auto slot = free_slot();
    if (full() || !slot) {
        r.status = PutStatus::Full;
        r.error = full_reason(max_);
        return r;
    }
    pin_stale_default(r.writes);
    insert_sorted(StoredTarget{*slot, std::move(t)});
    r.status = PutStatus::Created;
    r.slot = *slot;
    r.writes.save_slot = *slot;
    sync_default(r.writes);
    return r;
}

TargetTable::PutResult TargetTable::put(TargetConfig t) {
    PutResult r = put_record(std::move(t));
    if (r.status == PutStatus::Created || r.status == PutStatus::Updated) purge_skipped(r.writes);
    return r;
}

TargetTable::PutResult TargetTable::put_record(TargetConfig t) {
    PutResult r;
    if (!t.mac.empty()) {
        if (std::string m = normalize_mac(t.mac); !m.empty()) t.mac = std::move(m);
    }
    if (auto e = validate(t); !e.empty()) {
        r.error = std::move(e);
        return r;
    }
    t.id = t.effective_id();  // an id-less record is stored as "target"
    if (StoredTarget* st = find_mut(t.id)) {
        st->cfg = std::move(t);
        r.status = PutStatus::Updated;
        r.slot = st->slot;
        r.writes.save_slot = st->slot;
        return r;
    }
    const auto slot = free_slot();
    if (full() || !slot) {
        r.status = PutStatus::Full;
        r.error = full_reason(max_);
        return r;
    }
    pin_stale_default(r.writes);
    insert_sorted(StoredTarget{*slot, std::move(t)});
    r.status = PutStatus::Created;
    r.slot = *slot;
    r.writes.save_slot = *slot;
    sync_default(r.writes);
    return r;
}

bool TargetTable::remove(std::string_view id, TargetWrites& w) {
    const auto it = std::find_if(targets_.begin(), targets_.end(), [&](const StoredTarget& st) { return st.id() == id; });
    if (it == targets_.end()) return false;
    w.erase_slot = it->slot;
    targets_.erase(it);
    sync_default(w);
    purge_skipped(w);
    return true;
}

bool TargetTable::set_default(std::string_view id, TargetWrites& w) {
    const StoredTarget* st = find(id);
    if (st == nullptr) return false;
    if (stored_default_ != st->id()) {
        stored_default_ = st->id();
        w.default_id = stored_default_;
    }
    purge_skipped(w);
    return true;
}

std::string TargetTable::rename(std::string_view from, const std::string& to, TargetWrites& w) {
    StoredTarget* st = find_mut(from);
    if (st == nullptr) return "no target '" + std::string(from) + "'";
    if (to.empty()) return "id must not be empty";
    if (auto e = validate_target_id(to); !e.empty()) return e;
    if (st->id() == to) return {};
    if (find(to) != nullptr) return "a target with id '" + to + "' already exists";
    pin_stale_default(w);
    const bool was_default = is_default(from);
    st->cfg.id = to;
    w.save_slot = st->slot;
    if (was_default && stored_default_ != to) {
        stored_default_ = to;
        w.default_id = to;
    }
    purge_skipped(w);
    return {};
}

bool TargetTable::record_success(std::string_view id, const std::string& version, uint64_t at, TargetWrites& w) {
    StoredTarget* st = find_mut(id);
    if (st == nullptr) return false;
    st->cfg.last_ok_version = version.size() > kMaxVersionLen ? version.substr(0, kMaxVersionLen) : version;
    st->cfg.last_ok_at = at;
    w.save_slot = st->slot;
    return true;
}

}  // namespace dfu32
