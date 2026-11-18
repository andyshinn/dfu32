// SPDX-License-Identifier: MIT
#include "config/config.hpp"

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "dfu32_core/ap.hpp"

#include "esp_log.h"
#include "esp_random.h"
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
// Access-point fallback (SPEC.md §9.5). The password is stored in plaintext, like the
// station passphrase the WiFi driver keeps; it is never logged.
constexpr const char* kApEnabled = "ap_enabled";
constexpr const char* kApPassword = "ap_password";
constexpr const char* kApDelay = "ap_delay";
constexpr const char* kApIdle = "ap_idle";  // u32 seconds, 0 = never
constexpr const char* kToken = "token_sha256";

bool set_or_erase_str(nvs_handle_t h, const char* key, const std::string& v) {
    return v.empty() ? erase_key(h, key) : nvs_set_str(h, key, v.c_str()) == ESP_OK;
}

// ap_password_bad: the stored AP password failed validation and was sanitised away.
bool load(api::DeviceConfig& out, bool& ap_password_bad) {
    out = api::DeviceConfig{};
    ap_password_bad = false;
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
    uint8_t ap_on = 0;
    if (nvs_get_u8(h, kApEnabled, &ap_on) == ESP_OK) out.ap_enabled = ap_on != 0;
    get_str(h, kApPassword, out.ap_password);
    uint32_t ap_delay = 0;
    if (nvs_get_u32(h, kApDelay, &ap_delay) == ESP_OK) out.ap_delay_s = ap_delay;
    uint32_t ap_idle = 0;
    if (nvs_get_u32(h, kApIdle, &ap_idle) == ESP_OK) out.ap_idle_s = ap_idle;
    nvs_close(h);
    // SPEC.md §9.5: a bad delay or idle limit loads as the default, but a bad password turns AP mode
    // off instead of loading as "open", so a corrupt value can never open a protected AP.
    // Done before validate() below, which also checks the AP fields.
    const ap::LoadFixups fix = ap::sanitize_loaded(out.ap_enabled, out.ap_password, out.ap_delay_s, out.ap_idle_s);
    if (fix.delay_reset) {
        ESP_LOGW(TAG, "stored ap_delay is out of range; using %u s", static_cast<unsigned>(ap::kDefaultDelayS));
    }
    if (fix.idle_reset) {
        ESP_LOGW(TAG, "stored ap_idle is out of range; using %u s", static_cast<unsigned>(ap::kDefaultIdleS));
    }
    ap_password_bad = fix.password_invalid;
    if (fix.password_invalid) {
        ESP_LOGE(TAG, "stored AP password is invalid; AP mode stays off until `ap password <pw>` or "
                      "`ap password clear`");
    }
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

// keep_password: leave the stored ap_password as it is (see update_device).
bool save(const api::DeviceConfig& c, bool keep_password) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = set_or_erase_str(h, kHostname, c.hostname) && set_or_erase_str(h, kNtp, c.ntp_server) &&
                    nvs_set_u8(h, kWifiOff, c.wifi_off_dfu ? 1 : 0) == ESP_OK &&
                    nvs_set_u32(h, kDeadline, c.dfu_deadline_max_s) == ESP_OK &&
                    nvs_set_u8(h, kApEnabled, c.ap_enabled ? 1 : 0) == ESP_OK &&
                    (keep_password || set_or_erase_str(h, kApPassword, c.ap_password)) &&
                    nvs_set_u32(h, kApDelay, c.ap_delay_s) == ESP_OK &&
                    nvs_set_u32(h, kApIdle, c.ap_idle_s) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

}  // namespace

bool load_device(api::DeviceConfig& out) {
    bool bad = false;
    return load(out, bad);
}

bool save_device(const api::DeviceConfig& c) { return save(c, false); }

std::string update_device(const std::function<std::string(api::DeviceConfig&)>& edit, api::DeviceConfig* out) {
    // The console `ap` command and PUT /api/v1/config (SPEC.md §9.5) run on different
    // tasks; one lock around load-edit-save keeps either from losing the other's change.
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    api::DeviceConfig c;
    bool bad_pw = false;
    if (!load(c, bad_pw)) return "cannot read settings";
    // SPEC.md §9.5: an invalid stored AP password keeps AP mode off until `ap password
    // <pw>` or `ap password clear` stores a valid one. load() turned it into "" (open),
    // which `clear` also produces, so the edit gets a random valid stand-in instead: if
    // it is still there afterwards the edit left the password alone. Then the bad value
    // stays in NVS (a hostname save must not turn it into "open") and AP mode can't be
    // turned on, which would open the AP.
    std::string standin;
    if (bad_pw) {
        uint32_t r[4];
        esp_fill_random(r, sizeof r);
        char buf[33];
        std::snprintf(buf, sizeof buf, "%08lx%08lx%08lx%08lx", static_cast<unsigned long>(r[0]),
                      static_cast<unsigned long>(r[1]), static_cast<unsigned long>(r[2]),
                      static_cast<unsigned long>(r[3]));
        standin = buf;
        c.ap_password = standin;
    }
    if (auto e = edit(c); !e.empty()) return e;
    const bool keep_pw = bad_pw && c.ap_password == standin;
    if (keep_pw) {
        if (c.ap_enabled) return "the stored AP password is invalid; set `ap password <pw>` or `ap password clear` first";
        c.ap_password.clear();
    }
    if (auto e = api::validate(c); !e.empty()) return e;
    if (!save(c, keep_pw)) return "NVS write failed";
    if (out != nullptr) *out = std::move(c);
    return {};
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
