// SPDX-License-Identifier: MIT
#include "config/config.hpp"

#include <vector>

#include "nvs.h"

namespace dfu32::config {
namespace {

constexpr const char* kNs = "dfu32";
constexpr const char* kMac = "tgt0_mac";
constexpr const char* kType = "tgt0_type";
constexpr const char* kId = "tgt0_id";
constexpr const char* kName = "tgt0_name";
constexpr const char* kBoard = "tgt0_board";
constexpr const char* kUrl = "tgt0_url";
constexpr const char* kMaxApp = "tgt0_maxapp";

void get_str(nvs_handle_t h, const char* key, std::string& out) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return;
    std::vector<char> buf(len);
    if (nvs_get_str(h, key, buf.data(), &len) == ESP_OK) out.assign(buf.data());
}

bool set_str(nvs_handle_t h, const char* key, const std::string& v) {
    if (v.empty()) {
        const esp_err_t e = nvs_erase_key(h, key);
        return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;
    }
    return nvs_set_str(h, key, v.c_str()) == ESP_OK;
}

}  // namespace

bool load_target(TargetConfig& out) {
    out = TargetConfig{};
    nvs_handle_t h;
    const esp_err_t err = nvs_open(kNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;  // nothing saved yet
    if (err != ESP_OK) return false;
    get_str(h, kId, out.id);
    get_str(h, kName, out.name);
    get_str(h, kBoard, out.board);
    get_str(h, kMac, out.mac);
    get_str(h, kUrl, out.manifest_url);
    uint8_t type = 1;
    if (nvs_get_u8(h, kType, &type) == ESP_OK) out.addr_type = type;
    uint32_t max_app = 0;
    if (nvs_get_u32(h, kMaxApp, &max_app) == ESP_OK) out.max_app_size = max_app;
    nvs_close(h);
    out.mac = normalize_mac(out.mac);
    return true;
}

bool save_target(const TargetConfig& t) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = set_str(h, kId, t.id) && set_str(h, kName, t.name) && set_str(h, kBoard, t.board) &&
              set_str(h, kMac, t.mac) && set_str(h, kUrl, t.manifest_url) &&
              nvs_set_u8(h, kType, t.addr_type) == ESP_OK && nvs_set_u32(h, kMaxApp, t.max_app_size) == ESP_OK;
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool erase_target() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = true;
    for (const char* key : {kMac, kType, kId, kName, kBoard, kUrl, kMaxApp}) {
        const esp_err_t e = nvs_erase_key(h, key);
        ok = ok && (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND);
    }
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

}  // namespace dfu32::config
