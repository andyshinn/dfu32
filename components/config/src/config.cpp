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

}  // namespace dfu32::config
