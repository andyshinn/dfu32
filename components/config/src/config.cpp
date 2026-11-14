// SPDX-License-Identifier: MIT
#include "config/config.hpp"

#include <string>
#include <utility>
#include <vector>

#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs.h"

namespace dfu32::config {
namespace {

static_assert(CONFIG_DFU32_MAX_TARGETS >= 1 && CONFIG_DFU32_MAX_TARGETS <= kMaxTargetSlots,
              "CONFIG_DFU32_MAX_TARGETS must be 1..kMaxTargetSlots");

constexpr const char* TAG = "config";
constexpr const char* kNs = "dfu32";
// Each target is one JSON record in its own slot ("tgt0".."tgt9"), so saving a
// target is a single NVS entry write: either the old or the new record
// survives a failure or power cut, never a mix. Slot 0 is the key single-
// target builds used, so their target loads as slot 0 unchanged.
constexpr const char* kDefault = "tgt_def";  // id of the default target
// M1 builds stored only these two; read them as slot 0 when no record exists yet.
constexpr const char* kLegacyMac = "tgt0_mac";
constexpr const char* kLegacyType = "tgt0_type";

std::string slot_key(uint8_t slot) { return "tgt" + std::to_string(slot); }

bool get_str(nvs_handle_t h, const char* key, std::string& out) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return false;
    std::vector<char> buf(len);
    if (nvs_get_str(h, key, buf.data(), &len) != ESP_OK) return false;
    out.assign(buf.data());
    return true;
}

bool erase_key(nvs_handle_t h, const char* key) {
    const esp_err_t e = nvs_erase_key(h, key);
    return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;
}

// Nothing set at all: what a cleared single-target build could leave behind.
bool blank(const TargetConfig& t) {
    return t.id.empty() && t.mac.empty() && t.board.empty() && t.manifest_url.empty();
}

}  // namespace

size_t max_targets() { return CONFIG_DFU32_MAX_TARGETS; }

bool load_targets(TargetTable& out) {
    out = TargetTable(max_targets());
    nvs_handle_t h;
    const esp_err_t err = nvs_open(kNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;  // nothing saved yet
    if (err != ESP_OK) return false;
    for (size_t i = 0; i < kMaxTargetSlots; ++i) {
        const auto slot = static_cast<uint8_t>(i);
        TargetConfig t;
        std::string json;
        if (get_str(h, slot_key(slot).c_str(), json)) {
            if (!from_json(json, t)) {
                ESP_LOGE(TAG, "stored target record %s is corrupt; ignoring it (erased with the next target change)",
                         slot_key(slot).c_str());
                out.add_skipped(slot);
                continue;
            }
        } else if (slot == 0) {
            get_str(h, kLegacyMac, t.mac);
            uint8_t type = 1;
            if (nvs_get_u8(h, kLegacyType, &type) == ESP_OK && type <= 1) t.addr_type = type;
        } else {
            continue;
        }
        t.mac = normalize_mac(t.mac);
        if (const unsigned n = sanitize(t); n != 0) {
            ESP_LOGW(TAG, "stored target %s had %u invalid field(s); using defaults for them", slot_key(slot).c_str(),
                     n);
        }
        if (blank(t)) continue;
        if (!out.add_loaded(slot, std::move(t))) {
            ESP_LOGW(TAG, "stored target %s duplicates another target's id; ignoring it (erased with the next target change)",
                     slot_key(slot).c_str());
            out.add_skipped(slot);
        }
    }
    std::string def;
    get_str(h, kDefault, def);
    out.set_loaded_default(std::move(def));
    nvs_close(h);
    return true;
}

bool commit_targets(const TargetTable& table, const TargetWrites& w) {
    if (w.empty()) return true;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = true;
    bool legacy = false;  // slot 0 written or erased: the M1 keys are superseded
    if (w.save_slot) {
        const StoredTarget* st = table.at_slot(*w.save_slot);
        ok = st != nullptr && nvs_set_str(h, slot_key(*w.save_slot).c_str(), to_json(st->cfg).c_str()) == ESP_OK &&
             nvs_commit(h) == ESP_OK;
        legacy = legacy || *w.save_slot == 0;
    }
    if (ok && w.erase_slot) {
        ok = erase_key(h, slot_key(*w.erase_slot).c_str()) && nvs_commit(h) == ESP_OK;
        if (ok && *w.erase_slot == 0) {
            // Otherwise an M1 target would reappear as slot 0 on the next load.
            ok = erase_key(h, kLegacyMac) && erase_key(h, kLegacyType) && nvs_commit(h) == ESP_OK;
        }
    }
    if (ok && w.default_id) {
        ok = (w.default_id->empty() ? erase_key(h, kDefault) : nvs_set_str(h, kDefault, w.default_id->c_str()) == ESP_OK) &&
             nvs_commit(h) == ESP_OK;
    }
    for (const uint8_t slot : w.purge_slots) {
        if (!ok) break;
        ok = erase_key(h, slot_key(slot).c_str()) && nvs_commit(h) == ESP_OK;
        if (ok) ESP_LOGI(TAG, "erased skipped target record %s", slot_key(slot).c_str());
        legacy = legacy || slot == 0;
    }
    if (ok && legacy) {
        // Best-effort: the slot-0 record supersedes the M1 keys.
        erase_key(h, kLegacyMac);
        erase_key(h, kLegacyType);
        nvs_commit(h);
    }
    nvs_close(h);
    return ok;
}

bool load_target(const std::string& id, TargetConfig& out, bool& found) {
    out = TargetConfig{};
    found = false;
    TargetTable table;
    if (!load_targets(table)) return false;
    if (const StoredTarget* st = table.find(id)) {
        out = st->cfg;
        found = true;
    }
    return true;
}

bool load_default_target(TargetConfig& out, bool& found) {
    out = TargetConfig{};
    found = false;
    TargetTable table;
    if (!load_targets(table)) return false;
    if (const StoredTarget* st = table.default_target()) {
        out = st->cfg;
        found = true;
    }
    return true;
}

namespace {

constexpr const char* kHostname = "hostname";
constexpr const char* kNtp = "ntp_server";
constexpr const char* kWifiOff = "wifi_off_dfu";
constexpr const char* kDeadline = "dfu_deadline";
constexpr const char* kToken = "token_sha256";

bool set_or_erase_str(nvs_handle_t h, const char* key, const std::string& v) {
    return v.empty() ? erase_key(h, key) : nvs_set_str(h, key, v.c_str()) == ESP_OK;
}

}  // namespace

bool load_device(api::DeviceConfig& out) {
    out = api::DeviceConfig{};
    nvs_handle_t h;
    const esp_err_t err = nvs_open(kNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    get_str(h, kHostname, out.hostname);
    get_str(h, kNtp, out.ntp_server);
    uint8_t off = 1;
    if (nvs_get_u8(h, kWifiOff, &off) == ESP_OK) out.wifi_off_dfu = off != 0;
    uint32_t dl = 0;
    if (nvs_get_u32(h, kDeadline, &dl) == ESP_OK) out.dfu_deadline_max_s = dl;
    nvs_close(h);
    if (!api::validate(out).empty()) {
        ESP_LOGW(TAG, "stored device settings invalid (%s); using defaults where needed", api::validate(out).c_str());
        const api::DeviceConfig def;
        if (!validate_hostname(out.hostname).empty()) out.hostname = def.hostname;
        if (!api::validate_ntp_server(out.ntp_server).empty()) out.ntp_server = def.ntp_server;
        if (out.dfu_deadline_max_s < api::kMinDeadlineMaxS || out.dfu_deadline_max_s > api::kMaxDeadlineMaxS) {
            out.dfu_deadline_max_s = def.dfu_deadline_max_s;
        }
    }
    return true;
}

bool save_device(const api::DeviceConfig& c) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = set_or_erase_str(h, kHostname, c.hostname) && set_or_erase_str(h, kNtp, c.ntp_server) &&
                    nvs_set_u8(h, kWifiOff, c.wifi_off_dfu ? 1 : 0) == ESP_OK &&
                    nvs_set_u32(h, kDeadline, c.dfu_deadline_max_s) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool load_token_hash(std::array<uint8_t, 32>& out) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = out.size();
    const bool ok = nvs_get_blob(h, kToken, out.data(), &len) == ESP_OK && len == out.size();
    nvs_close(h);
    return ok;
}

bool save_token_hash(const std::array<uint8_t, 32>& hash) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_blob(h, kToken, hash.data(), hash.size()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool clear_token() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = erase_key(h, kToken) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool erase_all() {
    nvs_handle_t h;
    const esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    const bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

}  // namespace dfu32::config
