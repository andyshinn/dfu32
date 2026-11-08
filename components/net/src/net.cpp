// SPDX-License-Identifier: MIT
#include "net/net.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "dfu32_core/target_config.hpp"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace dfu32::net {
namespace {

constexpr const char* TAG = "net";
constexpr const char* kNvsNamespace = "dfu32";
constexpr EventBits_t kGotIp = BIT0;
constexpr uint32_t kBackoffMinMs = 1000;
constexpr uint32_t kBackoffMaxMs = 30000;
constexpr time_t kSaneEpoch = 1700000000;  // 2023-11; anything earlier means "never synced"

EventGroupHandle_t s_events = nullptr;
esp_netif_t* s_netif = nullptr;
esp_timer_handle_t s_retry = nullptr;
std::atomic<bool> s_init{false};
std::atomic<bool> s_stopped{false};
std::atomic<bool> s_want_connect{true};
std::atomic<uint32_t> s_backoff_ms{kBackoffMinMs};
std::atomic<uint32_t> s_reconnects{0};
std::atomic<uint8_t> s_last_reason{0};

// Link state as seen from our side, so a retry never issues a second
// esp_wifi_connect() while one is in flight or already associated.
enum Link : uint8_t { kIdle, kConnecting, kAssociated };
std::atomic<uint8_t> s_link{kIdle};
// set_credentials() disconnects on purpose. Its STA_DISCONNECTED arrives later
// on the event task; swallow that one event (if it comes within 2 s) so it
// doesn't count as a drop or arm a retry against the new connection.
std::atomic<int64_t> s_swallow_until_us{0};
constexpr int64_t kSwallowWindowUs = 2000000;
// Set once in init(); lwIP's SNTP keeps the server name pointer.
char s_hostname[32] = {};
char s_ntp_server[64] = "pool.ntp.org";

bool provisioned() {
    wifi_config_t cfg{};
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != 0;
}

void connect_now() {
    if (s_stopped || !s_want_connect || s_link != kIdle || !provisioned()) return;
    s_link = kConnecting;
    const esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) return;  // CONN: already connecting
    s_link = kIdle;
    ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
}

// Disconnect on purpose (new credentials, forget).
void local_disconnect() {
    esp_timer_stop(s_retry);
    if (s_link != kIdle) s_swallow_until_us = esp_timer_get_time() + kSwallowWindowUs;
    esp_wifi_disconnect();  // synchronous: the driver is idle once it returns
    s_link = kIdle;
    xEventGroupClearBits(s_events, kGotIp);
}

void retry_cb(void*) { connect_now(); }

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_link = kIdle;
        s_backoff_ms = kBackoffMinMs;
        connect_now();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_link = kAssociated;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* d = static_cast<const wifi_event_sta_disconnected_t*>(data);
        const int64_t until = s_swallow_until_us.exchange(0);
        if (until != 0 && esp_timer_get_time() <= until) return;  // our own local_disconnect()
        s_last_reason = d->reason;
        s_link = kIdle;
        xEventGroupClearBits(s_events, kGotIp);
        if (s_stopped || !s_want_connect || !provisioned()) return;
        const uint32_t wait = s_backoff_ms;
        ++s_reconnects;
        ESP_LOGW(TAG, "WiFi disconnected (reason %u); retrying in %u ms", d->reason, static_cast<unsigned>(wait));
        esp_timer_stop(s_retry);
        esp_timer_start_once(s_retry, uint64_t{wait} * 1000);
        s_backoff_ms = std::min(wait * 2, kBackoffMaxMs);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* e = static_cast<const ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "WiFi connected, IP " IPSTR ", http://%s.local/", IP2STR(&e->ip_info.ip), s_hostname);
        s_backoff_ms = kBackoffMinMs;
        xEventGroupSetBits(s_events, kGotIp);
    }
}

void load_settings() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(s_hostname, sizeof(s_hostname), "dfu32-%02x%02x", mac[4], mac[5]);
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
    // A stored hostname must be a legal DHCP/mDNS label; anything else (or
    // too long for the buffer) keeps the default.
    char host[sizeof(s_hostname)] = {};
    size_t len = sizeof(host);
    if (nvs_get_str(h, "hostname", host, &len) == ESP_OK) {
        const std::string why = validate_hostname(host);
        if (why.empty()) {
            std::memcpy(s_hostname, host, sizeof(s_hostname));
        } else {
            ESP_LOGW(TAG, "ignoring stored hostname \"%s\": %s", host, why.c_str());
        }
    }
    char ntp[sizeof(s_ntp_server)] = {};
    len = sizeof(ntp);
    if (nvs_get_str(h, "ntp_server", ntp, &len) == ESP_OK && ntp[0] != 0) {
        std::memcpy(s_ntp_server, ntp, sizeof(s_ntp_server));
    }
    nvs_close(h);
}

void start_mdns() {
    esp_err_t err = mdns_init();
    if (err == ESP_OK) err = mdns_hostname_set(s_hostname);
    if (err == ESP_OK) err = mdns_instance_name_set("DFU32");
    if (err != ESP_OK) ESP_LOGW(TAG, "mDNS unavailable: %s", esp_err_to_name(err));
}

void start_sntp() {
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server);
    const esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) ESP_LOGW(TAG, "SNTP unavailable: %s", esp_err_to_name(err));
}

#define NET_CHECK(expr)                                                         \
    do {                                                                        \
        const esp_err_t err_ = (expr);                                          \
        if (err_ != ESP_OK) {                                                   \
            ESP_LOGE(TAG, "%s failed: %s", #expr, esp_err_to_name(err_));       \
            return false;                                                       \
        }                                                                       \
    } while (0)

}  // namespace

bool init() {
    // One shot: a half-initialised driver can't be brought up a second time
    // (esp_netif_create_default_wifi_sta asserts), so a failed init stays failed.
    static bool attempted = false;
    if (attempted) return s_init;
    attempted = true;
    load_settings();
    s_events = xEventGroupCreate();
    if (s_events == nullptr) return false;
    NET_CHECK(esp_netif_init());
    const esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(loop));
        return false;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == nullptr) return false;
    const wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    NET_CHECK(esp_wifi_init(&wcfg));
    NET_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    NET_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr, nullptr));
    NET_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr, nullptr));
    esp_timer_create_args_t targs{};
    targs.callback = &retry_cb;
    targs.name = "wifi_retry";
    NET_CHECK(esp_timer_create(&targs, &s_retry));
    NET_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_netif_set_hostname(s_netif, s_hostname);
    // WiFi always starts at boot (SPEC.md §7.3 boot invariant), provisioned or not.
    NET_CHECK(esp_wifi_start());
    start_mdns();
    start_sntp();
    s_init = true;
    if (provisioned()) {
        ESP_LOGI(TAG, "hostname %s; connecting to stored network", s_hostname);
    } else {
        ESP_LOGW(TAG, "hostname %s; no WiFi credentials yet: use `wifi <ssid> <password>`", s_hostname);
    }
    return true;
}

Status status() {
    Status s;
    s.initialised = s_init;
    s.hostname = s_hostname;
    if (!s_init) return s;
    wifi_config_t cfg{};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        const auto* ssid = reinterpret_cast<const char*>(cfg.sta.ssid);
        s.ssid.assign(ssid, strnlen(ssid, sizeof(cfg.sta.ssid)));
    }
    s.provisioned = !s.ssid.empty();
    s.stopped = s_stopped;
    s.connected = (xEventGroupGetBits(s_events) & kGotIp) != 0;
    if (s.connected) {
        esp_netif_ip_info_t ip{};
        if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
            s.ip = buf;
        }
        int rssi = 0;
        if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) s.rssi = rssi;
    }
    s.last_disconnect_reason = s_last_reason;
    s.reconnects = s_reconnects;
    s.time_synced = unix_time() != 0;
    return s;
}

bool set_credentials(const std::string& ssid, const std::string& password) {
    if (!s_init || ssid.empty() || ssid.size() > 32 || password.size() > 64) return false;
    wifi_config_t cfg{};
    std::memcpy(cfg.sta.ssid, ssid.data(), ssid.size());
    std::memcpy(cfg.sta.password, password.data(), password.size());
    // Refuse to fall back to an open network when a password was given.
    cfg.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;  // unused while WPA3 is off (sdkconfig.defaults)
    s_want_connect = false;
    local_disconnect();
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        return false;
    }
    s_want_connect = true;
    s_backoff_ms = kBackoffMinMs;
    connect_now();
    return true;
}

bool forget() {
    if (!s_init) return false;
    s_want_connect = false;
    local_disconnect();
    wifi_config_t cfg{};
    const bool ok = esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK;
    s_want_connect = true;  // a later set_credentials() connects again
    return ok;
}

bool try_credentials(const std::string& ssid, const std::string& password) {
    if (!s_init) return false;
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return false;
    const bool ok = set_credentials(ssid, password);
    // Back to flash for everything else; this config stays RAM-only until
    // commit_credentials() writes it again.
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    return ok;
}

bool commit_credentials() {
    if (!s_init) return false;
    wifi_config_t cfg{};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK || cfg.sta.ssid[0] == 0) return false;
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);  // storage is FLASH again
    if (err != ESP_OK) ESP_LOGE(TAG, "saving WiFi credentials: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

bool stored_credentials(std::string& ssid, std::string& password) {
    if (!s_init) return false;
    wifi_config_t cfg{};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK || cfg.sta.ssid[0] == 0) return false;
    ssid.assign(reinterpret_cast<const char*>(cfg.sta.ssid), strnlen(reinterpret_cast<const char*>(cfg.sta.ssid), 32));
    password.assign(reinterpret_cast<const char*>(cfg.sta.password),
                    strnlen(reinterpret_cast<const char*>(cfg.sta.password), 64));
    return true;
}

bool wait_connected(uint32_t timeout_ms) {
    if (!s_init) return false;
    return (xEventGroupWaitBits(s_events, kGotIp, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & kGotIp) != 0;
}

bool stop() {
    if (!s_init) return false;
    s_stopped = true;
    esp_timer_stop(s_retry);
    xEventGroupClearBits(s_events, kGotIp);
    const esp_err_t err = esp_wifi_stop();
    s_link = kIdle;
    if (err != ESP_OK) ESP_LOGE(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "WiFi stopped for the BLE transfer");
    return err == ESP_OK;
}

bool restore(uint32_t timeout_ms) {
    if (!s_init) return false;
    s_stopped = false;
    s_backoff_ms = kBackoffMinMs;
    const esp_err_t err = esp_wifi_start();  // STA_START -> connect
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "WiFi restarted");
    return wait_connected(timeout_ms);
}

uint64_t unix_time() {
    const time_t now = time(nullptr);
    return now >= kSaneEpoch ? static_cast<uint64_t>(now) : 0;
}

void advertise_http(uint16_t port) {
    if (!s_init) return;
    mdns_txt_item_t txt[] = {{"path", "/api/v1/"}, {"board", CONFIG_DFU32_BOARD_NAME}};
    const esp_err_t err = mdns_service_add(nullptr, "_http", "_tcp", port, txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) ESP_LOGW(TAG, "mDNS _http._tcp: %s", esp_err_to_name(err));
}

}  // namespace dfu32::net
