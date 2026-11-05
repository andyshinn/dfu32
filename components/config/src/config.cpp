// SPDX-License-Identifier: MIT
#include "config/config.hpp"

#include <vector>

#include "esp_log.h"
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

}  // namespace dfu32::config
