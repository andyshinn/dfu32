// SPDX-License-Identifier: MIT
#include "config/config.hpp"

#include <cstdio>
#include <mutex>
#include <vector>

#include "dfu32_core/ap.hpp"

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

namespace dfu32::config {
namespace {

constexpr const char* TAG = "config";
constexpr const char* kNs = "dfu32";
// Target 0 is one JSON record, so a save is a single NVS entry write: either
// the old or the new record survives a failure or power cut, never a mix.
constexpr const char* kTarget = "tgt0";
// M1 builds stored only these two; read them when no record exists yet.
constexpr const char* kLegacyMac = "tgt0_mac";
constexpr const char* kLegacyType = "tgt0_type";

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

}  // namespace

bool load_target(TargetConfig& out) {
    out = TargetConfig{};
    nvs_handle_t h;
    const esp_err_t err = nvs_open(kNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;  // nothing saved yet
    if (err != ESP_OK) return false;
    std::string json;
    if (get_str(h, kTarget, json)) {
        if (!from_json(json, out)) ESP_LOGE(TAG, "stored target record is corrupt; ignoring it");
    } else {
        get_str(h, kLegacyMac, out.mac);
        uint8_t type = 1;
        if (nvs_get_u8(h, kLegacyType, &type) == ESP_OK && type <= 1) out.addr_type = type;
    }
    nvs_close(h);
    out.mac = normalize_mac(out.mac);
    if (const unsigned n = sanitize(out); n != 0) {
        ESP_LOGW(TAG, "stored target had %u invalid field(s); using defaults for them", n);
    }
    return true;
}

bool save_target(const TargetConfig& t) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_str(h, kTarget, to_json(t).c_str()) == ESP_OK && nvs_commit(h) == ESP_OK;
    if (ok) {
        // The record supersedes the M1 keys; dropping them is best-effort.
        erase_key(h, kLegacyMac);
        erase_key(h, kLegacyType);
        nvs_commit(h);
    }
    nvs_close(h);
    return ok;
}

bool erase_target() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = erase_key(h, kTarget) && erase_key(h, kLegacyMac) && erase_key(h, kLegacyType) &&
                    nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
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
    nvs_close(h);
    // SPEC.md §9.5: a bad delay loads as the default, but a bad password turns AP mode
    // off instead of loading as "open", so a corrupt value can never open a protected AP.
    // Done before validate() below, which also checks the AP fields.
    const ap::LoadFixups fix = ap::sanitize_loaded(out.ap_enabled, out.ap_password, out.ap_delay_s);
    if (fix.delay_reset) {
        ESP_LOGW(TAG, "stored ap_delay is out of range; using %u s", static_cast<unsigned>(ap::kDefaultDelayS));
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
                    nvs_set_u32(h, kApDelay, c.ap_delay_s) == ESP_OK && nvs_commit(h) == ESP_OK;
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
