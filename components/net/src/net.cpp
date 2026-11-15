// SPDX-License-Identifier: MIT
#include "net/net.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

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
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
#include "captive_dns.hpp"
#include "dfu32_core/ap.hpp"
#include "lwip/sockets.h"
#endif

namespace dfu32::net {
namespace {

using Lock = std::lock_guard<std::mutex>;

constexpr const char* TAG = "net";
constexpr const char* kNvsNamespace = "dfu32";
constexpr EventBits_t kGotIp = BIT0;
constexpr uint32_t kBackoffMinMs = 1000;
constexpr uint32_t kBackoffMaxMs = 30000;
constexpr uint32_t kRadioBusyRetryMs = 1000;
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
// Automatic station retries paused for AP clients (SPEC.md §9.5). Always false
// without SoftAP support.
std::atomic<bool> s_paused{false};

// The radio mutex. Every esp_wifi_set_mode / set_config / connect / disconnect /
// scan_start / set_storage / start / stop runs under it, so the AP controller,
// a blocking scan and the public calls never interleave driver changes. The
// event task and esp_timer callbacks only try_lock it and come back later.
// Lock order: s_wifi_mu before s_ctrl_mu; no esp_wifi call under s_ctrl_mu.
std::mutex s_wifi_mu;

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

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
// Sticky: WIFI_EVENT_AP_START seen since boot (OTA confirm, SPEC.md §12.3).
constexpr EventBits_t kApStarted = BIT1;
constexpr uint32_t kEvalBusyRetryMs = 200;

// Guarded by s_ctrl_mu (short holds, no driver calls inside).
std::mutex s_ctrl_mu;
ap::Controller s_ctrl;
ApSettings s_ap;       // latest settings: what status() reports
ApSettings s_ap_live;  // in force; s_ap moves here kApplyGraceMs after set_ap()
bool s_ap_pending = false;
uint64_t s_apply_at_ms = 0;
uint32_t s_ap_gen = 0;             // bumped when s_ap_live's password changes
uint64_t s_up_at_ms = ap::kNever;  // standby: when the AP is due (status up_in_s)

// Guarded by s_wifi_mu.
uint32_t s_ap_applied_gen = 0;  // generation of the AP config loaded into the driver
bool s_dns_on = false;

esp_netif_t* s_ap_netif = nullptr;
bool s_ap_usable = false;  // AP netif made and config preloaded at init; else AP mode acts as off
esp_timer_handle_t s_ap_eval = nullptr;
std::atomic<bool> s_ap_mode{false};     // driver mode includes the AP (false while stopped)
std::atomic<bool> s_ap_running{false};  // between AP_START and AP_STOP
std::atomic<bool> s_ap_fresh{false};    // AP_START not yet followed by set_inactive_time
std::atomic<uint8_t> s_ap_clients{0};
std::atomic<uint8_t> s_last_sta_channel{0};
std::atomic<const char*> s_ap_fault{nullptr};
std::atomic<ApObserver> s_observer{nullptr};
#endif

bool got_ip() { return (xEventGroupGetBits(s_events) & kGotIp) != 0; }

bool provisioned() {
    wifi_config_t cfg{};
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != 0;
}

void arm_retry(uint32_t ms) {
    esp_timer_stop(s_retry);
    esp_timer_start_once(s_retry, uint64_t{ms} * 1000);
}

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
uint64_t now_ms() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }

// Runs the AP controller (ap_eval_cb) on the esp_timer task now. Everything
// that can change its answer kicks it; it re-arms itself for the next instant
// its answer could change on its own (SPEC.md §9.5).
void kick() {
    if (s_ap_eval == nullptr) return;
    // stop + start isn't atomic: a running ap_eval_cb (esp_timer task, maybe on
    // the other core, or resuming from sync_dns) can re-arm itself in between,
    // and start_once on an armed timer fails. The kick would then wait for that
    // later instant with the facts the running pass read before this change
    // (a joined phone not pausing the station). So stop and start again.
    for (int i = 0; i < 3; ++i) {
        esp_timer_stop(s_ap_eval);
        if (esp_timer_start_once(s_ap_eval, 0) == ESP_OK) return;
    }
}

bool join_window_open() {
    Lock lk(s_ctrl_mu);
    return s_ctrl.join_window_open(now_ms());
}
#else
void kick() {}
#endif

void connect_now() {
    if (s_stopped || !s_want_connect || s_link != kIdle || !provisioned()) return;
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    // Strict pause (SPEC.md §9.5): no automatic attempt while a phone is on the
    // AP, unless set_credentials() or restore() opened the join window.
    if (s_paused && !join_window_open()) return;
#endif
    std::unique_lock<std::mutex> radio(s_wifi_mu, std::try_to_lock);
    if (!radio.owns_lock()) {  // a scan or an AP change has the radio
        arm_retry(kRadioBusyRetryMs);
        return;
    }
    s_link = kConnecting;
    const esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) return;  // CONN: already connecting
    s_link = kIdle;
    ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    arm_retry(s_backoff_ms);  // e.g. a driver scan still running: try again, don't drop it
}

// Disconnect on purpose (new credentials, forget). Under s_wifi_mu.
void local_disconnect() {
    esp_timer_stop(s_retry);
    if (s_link != kIdle) s_swallow_until_us = esp_timer_get_time() + kSwallowWindowUs;
    esp_wifi_disconnect();  // synchronous: the driver is idle once it returns
    s_link = kIdle;
    xEventGroupClearBits(s_events, kGotIp);
}

// Under s_wifi_mu. The driver storage stays RAM for good, so the AP config (and
// its password) never lands in the driver's NVS and AP up/down writes no flash;
// only the station config is written through (SPEC.md §9.5).
esp_err_t set_sta_config(wifi_config_t& cfg) {
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    return err;
}

void retry_cb(void*) { connect_now(); }

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
bool mode_has_ap() {
    wifi_mode_t m = WIFI_MODE_NULL;
    return esp_wifi_get_mode(&m) == ESP_OK && (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA);
}

// Clients associated right now, re-read from the driver at every evaluation,
// never added up from events (SPEC.md §9.5). Under s_wifi_mu, which also guards
// the static list (kept off the esp_timer task's stack).
uint8_t ap_clients() {
    static wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? static_cast<uint8_t>(list.num) : 0;
}

// Under s_wifi_mu (AP mode on). Loads the AP config into the driver (SPEC.md
// §9.5 "What the AP looks like").
esp_err_t apply_ap_cfg() {
    wifi_config_t c{};
    const size_t n = strnlen(s_hostname, sizeof(c.ap.ssid));
    std::memcpy(c.ap.ssid, s_hostname, n);
    c.ap.ssid_len = static_cast<uint8_t>(n);
    wifi_country_t country{};
    esp_wifi_get_country(&country);
    c.ap.channel = ap::ap_channel(s_last_sta_channel, country.schan, country.nchan);
    uint32_t gen;
    {
        Lock lk(s_ctrl_mu);
        // Validated: 8-63 printable ASCII or empty, so it fits and has no NUL.
        std::memcpy(c.ap.password, s_ap_live.password.data(), s_ap_live.password.size());
        gen = s_ap_gen;
    }
    c.ap.authmode = c.ap.password[0] != 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    c.ap.max_connection = ap::kMaxClients;
    c.ap.beacon_interval = 100;
    c.ap.csa_count = 3;             // the channel switch when the AP follows the station
    c.ap.pmf_cfg.required = false;  // offered, not required
    c.ap.gtk_rekey_interval = 0;    // no group rekeying
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &c);
    // The local copy carries the password; the barrier keeps the wipe.
    std::memset(&c, 0, sizeof(c));
    asm volatile("" : : "r"(&c) : "memory");
    if (err == ESP_OK) {
        s_ap_applied_gen = gen;
        // set_config may reset the driver's inactivity limit: re-apply it, so a
        // gone phone can't hold the station-retry pause past 60 s (§9.5).
        s_ap_fresh = true;
    }
    return err;
}

// Under s_ctrl_mu. Only a password change needs the driver (and a deauth); a
// delay or idle limit change just moves the controller's timers (§9.5).
void promote_settings() {
    if (s_ap_live.password != s_ap.password) ++s_ap_gen;
    s_ap_live = s_ap;
    s_ap_pending = false;
}

// Under s_ctrl_mu. Without its netif or preloaded config the AP can't run, so
// AP mode acts as off (the fault shows in status, §9.5).
ap::Settings live_settings() { return {s_ap_live.enabled && s_ap_usable, s_ap_live.delay_s, s_ap_live.idle_s}; }

// Under s_wifi_mu. The captive DNS follows the AP (SPEC.md §9.5): on once it
// has started, off when it is down. Level-triggered, so a failed start is
// retried at the next evaluation.
void sync_dns(bool want) {
    if (want == s_dns_on) return;
    if (want) {
        s_dns_on = captive_dns_start();
        if (!s_dns_on) ESP_LOGW(TAG, "captive DNS unavailable");
    } else {
        captive_dns_stop();
        s_dns_on = false;
    }
}

void notify(ApEvent e) {
    if (ApObserver fn = s_observer) fn(e);
}

// The AP controller, on the esp_timer task: read the facts, ask
// ap::Controller::step(), apply its answer (SPEC.md §9.5). Never blocks on the
// radio mutex.
void ap_eval_cb(void*) {
    std::unique_lock<std::mutex> radio(s_wifi_mu, std::try_to_lock);
    if (!radio.owns_lock()) {  // a scan or a public call has the radio
        esp_timer_start_once(s_ap_eval, uint64_t{kEvalBusyRetryMs} * 1000);
        return;
    }
    if (s_stopped) return;  // radio handed over: restore() reconciles everything
    const uint64_t now = now_ms();
    ap::Facts f;
    f.provisioned = provisioned();
    f.sta_has_ip = got_ip();
    f.ap_up = mode_has_ap();
    f.clients = f.ap_up ? ap_clients() : 0;
    ap::Decision d;
    uint32_t gen;
    {
        Lock lk(s_ctrl_mu);
        if (s_ap_pending && now >= s_apply_at_ms) promote_settings();
        d = s_ctrl.step(live_settings(), f, now);
        // Not yet due settings wait here, so an event in between can't apply
        // them before the HTTP or console reply is out.
        if (s_ap_pending) d.next_eval_ms = std::min(d.next_eval_ms, s_apply_at_ms);
        s_up_at_ms = d.up_in_ms < 0 ? ap::kNever : now + static_cast<uint64_t>(d.up_in_ms);
        gen = s_ap_gen;
    }

    if (d.action == ap::Action::BringUp) {
        // set_config needs the AP in the mode, so the AP starts with the config
        // last loaded; if the password changed since, a phone that joined in
        // that moment must not stay on from the old security (§9.5).
        const bool stale = s_ap_applied_gen != gen;
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) err = apply_ap_cfg();
        if (err == ESP_OK) {
            if (stale) esp_wifi_deauth_sta(0);
            s_ap_fault = nullptr;
            ESP_LOGI(TAG, "access point \"%s\" up: %s", s_hostname, ap::kUrl);
        } else {
            // Logged, shown in status and retried every 10 s; the station is never affected.
            ESP_LOGE(TAG, "access point start: %s", esp_err_to_name(err));
            esp_wifi_set_mode(WIFI_MODE_STA);
            s_ap_fault = "start";
            Lock lk(s_ctrl_mu);
            s_ctrl.bringup_failed(now);
            d.next_eval_ms = std::min(d.next_eval_ms, now + ap::kBringUpRetryMs);
        }
    } else if (d.action == ap::Action::TakeDown) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "access point down%s", d.why == ap::Why::Idle ? ": idle limit reached" : "");
    } else if (f.ap_up && s_ap_applied_gen != gen) {
        if (apply_ap_cfg() == ESP_OK) {
            // A new password while up: nobody stays on from the old security.
            esp_wifi_deauth_sta(0);
            s_ap_fault = nullptr;
            ESP_LOGI(TAG, "access point security changed; clients must rejoin");
        } else {
            // The old security stays on air until this succeeds: retry, don't
            // wait for an event that may never come (a paused AP is quiet).
            ESP_LOGE(TAG, "access point security change failed");
            s_ap_fault = "start";
            d.next_eval_ms = std::min(d.next_eval_ms, now + ap::kBringUpRetryMs);
        }
    }

    const bool on = mode_has_ap();
    // Needs WiFi started, so it is re-applied after every AP_START: a phone gone
    // without a deauth can't hold the station-retry pause.
    if (on && s_ap_fresh.exchange(false)) esp_wifi_set_inactive_time(WIFI_IF_AP, ap::kInactiveS);
    sync_dns(on && s_ap_running);
    if (on && s_ap_running && !s_dns_on) d.next_eval_ms = std::min(d.next_eval_ms, now + ap::kBringUpRetryMs);

    const bool was_paused = s_paused.exchange(d.pause_sta);
    const bool resume = was_paused && !d.pause_sta;
    if (d.pause_sta && !was_paused) {
        // Strict pause: no retry timer stays armed (one armed in the join
        // window would only no-op in connect_now()).
        esp_timer_stop(s_retry);
        ESP_LOGI(TAG, "station retries paused while a phone is on the access point");
    }
    if (resume) {
        s_backoff_ms = kBackoffMinMs;  // the last client left: retry at once
    } else if (!d.pause_sta && f.provisioned && !f.sta_has_ip && s_want_connect && s_link == kIdle &&
               !esp_timer_is_active(s_retry)) {
        // Self-heal: a retry skipped while paused or while the radio was busy.
        esp_timer_start_once(s_retry, uint64_t{s_backoff_ms} * 1000);
    }

    const uint8_t clients = on ? f.clients : 0;
    const uint8_t had = s_ap_clients.exchange(clients);
    const bool was_on = s_ap_mode.exchange(on);
    // The count only: client logs never carry a MAC (§9.5).
    if (clients != had) ESP_LOGI(TAG, "access point: %u of %u clients", unsigned{clients}, unsigned{ap::kMaxClients});
    if (d.next_eval_ms != ap::kNever) {
        esp_timer_start_once(s_ap_eval, (d.next_eval_ms > now ? d.next_eval_ms - now : 0) * 1000);
    }
    radio.unlock();
    if (resume) connect_now();
    if (on != was_on) notify(on ? ApEvent::Up : ApEvent::Down);
    if (clients > had) notify(ApEvent::ClientJoined);
}

// Under s_wifi_mu, while stopped, before esp_wifi_start(): puts the driver's
// mode and AP config in line with the settings, so a disabled AP never comes
// back, even briefly (SPEC.md §9.5 "Interactions").
void reconcile_ap() {
    const uint64_t now = now_ms();
    ap::Facts f;  // as if running, station without an IP: what start-up will look like
    f.provisioned = provisioned();
    f.ap_up = mode_has_ap();
    ap::Decision d;
    uint32_t gen;
    {
        Lock lk(s_ctrl_mu);
        if (s_ap_pending) promote_settings();  // settings changed while stopped apply now
        d = s_ctrl.step(live_settings(), f, now);
        // Also opens the join window: a phone that rejoins the AP doesn't pause the §7.3 verification.
        s_ctrl.restored(now);
        gen = s_ap_gen;
    }
    esp_err_t err = ESP_OK;
    if (f.ap_up && !d.want_up) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    } else if (d.want_up && (!f.ap_up || s_ap_applied_gen != gen)) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) err = apply_ap_cfg();
        if (err == ESP_OK) {
            s_ap_fault = nullptr;  // the controller sees it up and won't BringUp (which clears it)
        } else {
            esp_wifi_set_mode(WIFI_MODE_STA);
            s_ap_fault = "start";
            Lock lk(s_ctrl_mu);
            s_ctrl.bringup_failed(now);
        }
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "access point at restore: %s", esp_err_to_name(err));
}
#endif  // CONFIG_ESP_WIFI_SOFTAP_SUPPORT

// On the event task (2304 B stack): no set_mode/set_config or blocking calls
// here. The AP work goes through kick().
void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_link = kIdle;
        s_backoff_ms = kBackoffMinMs;
        connect_now();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_link = kAssociated;
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
        // The AP's channel while the station is idle (§9.5): a router that comes
        // back on the same channel then causes no channel switch.
        s_last_sta_channel = static_cast<const wifi_event_sta_connected_t*>(data)->channel;
#endif
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* d = static_cast<const wifi_event_sta_disconnected_t*>(data);
        const int64_t until = s_swallow_until_us.exchange(0);
        if (until != 0 && esp_timer_get_time() <= until) return;  // our own local_disconnect()
        s_last_reason = d->reason;
        s_link = kIdle;
        const EventBits_t was = xEventGroupClearBits(s_events, kGotIp);  // bits before clearing
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
        if ((was & kGotIp) != 0) {
            Lock lk(s_ctrl_mu);
            s_ctrl.ip_lost(now_ms());  // starts the "without an IP since" clock
        }
        kick();
#else
        (void)was;
#endif
        if (s_stopped || !s_want_connect || !provisioned()) return;
        ++s_reconnects;
        if (s_paused) {  // strict pause (§9.5): no retry timer; the AP controller resumes them
            ESP_LOGW(TAG, "WiFi disconnected (reason %u); retries paused", d->reason);
            return;
        }
        const uint32_t wait = s_backoff_ms;
        ESP_LOGW(TAG, "WiFi disconnected (reason %u); retrying in %u ms", d->reason, static_cast<unsigned>(wait));
        arm_retry(wait);
        s_backoff_ms = std::min(wait * 2, kBackoffMaxMs);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* e = static_cast<const ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "WiFi connected, IP " IPSTR ", http://%s.local/", IP2STR(&e->ip_info.ip), s_hostname);
        s_backoff_ms = kBackoffMinMs;
        xEventGroupSetBits(s_events, kGotIp);
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
        {
            Lock lk(s_ctrl_mu);
            s_ctrl.ip_got();
        }
        kick();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        xEventGroupSetBits(s_events, kApStarted);
        s_ap_running = true;
        s_ap_fresh = true;
        kick();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        s_ap_running = false;
        s_ap_fresh = false;
        s_ap_clients = 0;
        kick();
    } else if (base == WIFI_EVENT && (id == WIFI_EVENT_AP_STACONNECTED || id == WIFI_EVENT_AP_STADISCONNECTED)) {
        kick();  // the controller re-reads the count and logs it (never the MAC)
    } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        const auto* e = static_cast<const ip_event_assigned_ip_to_client_t*>(data);
        ESP_LOGD(TAG, "access point client got " IPSTR, IP2STR(&e->ip));
#endif
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

bool init(const ApSettings& ap_settings) {
    // One shot: a half-initialised driver can't be brought up a second time
    // (esp_netif_create_default_wifi_sta asserts), so a failed init stays failed.
    static bool attempted = false;
    if (attempted) return s_init;
    attempted = true;
    load_settings();
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    {
        // config already sanitised them; checked again so a bad value can never
        // open a protected AP (SPEC.md §9.5).
        ApSettings a = ap_settings;
        const ap::LoadFixups fix = ap::sanitize_loaded(a.enabled, a.password, a.delay_s, a.idle_s);
        if (fix.password_invalid) ESP_LOGE(TAG, "AP password invalid; AP mode stays off");
        Lock lk(s_ctrl_mu);
        s_ap = a;
        s_ap_live = a;
    }
#else
    (void)ap_settings;
#endif
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
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    // Made once, even with AP mode off, so set_ap() can turn it on at runtime.
    // Nothing AP-related fails init (a failing init turns the API off and rolls
    // back a pending OTA, §12.3): the fault shows in status instead.
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == nullptr) {
        ESP_LOGE(TAG, "access point netif unavailable");
        s_ap_fault = "netif";
    }
#endif
    const wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    NET_CHECK(esp_wifi_init(&wcfg));
    // RAM for good; set_sta_config() writes the station config through (§9.5).
    NET_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    NET_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr, nullptr));
    NET_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr, nullptr));
    esp_timer_create_args_t targs{};
    targs.callback = &retry_cb;
    targs.name = "wifi_retry";
    NET_CHECK(esp_timer_create(&targs, &s_retry));
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, &on_event, nullptr, nullptr);
    targs.callback = &ap_eval_cb;
    targs.name = "wifi_ap";
    if (esp_timer_create(&targs, &s_ap_eval) != ESP_OK) s_ap_eval = nullptr;
    // Preload the AP config before esp_wifi_start(), so a later AP start never
    // beacons the driver's default open network (§9.5). No radio mutex yet:
    // nothing else touches the driver before it starts.
    bool want_ap = false;
    if (s_ap_netif != nullptr && s_ap_eval != nullptr) {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) err = apply_ap_cfg();
        s_ap_usable = err == ESP_OK;
        if (!s_ap_usable) {
            ESP_LOGE(TAG, "access point config: %s", esp_err_to_name(err));
            s_ap_fault = "start";
        }
        // AP mode on with no network stored: the AP starts with the station (§7.3 layer 5).
        Lock lk(s_ctrl_mu);
        want_ap = s_ap_usable && s_ap_live.enabled;
    }
    want_ap = want_ap && !provisioned();
    NET_CHECK(esp_wifi_set_mode(want_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA));
#else
    NET_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#endif
    esp_netif_set_hostname(s_netif, s_hostname);
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    {
        Lock lk(s_ctrl_mu);
        s_ctrl.boot(now_ms());
    }
#endif
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
    kick();
    return true;
}

Status status() {
    Status s;
    s.initialised = s_init;
    s.hostname = s_hostname;
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    s.ap.supported = true;
#endif
    if (!s_init) return s;
    wifi_config_t cfg{};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        const auto* ssid = reinterpret_cast<const char*>(cfg.sta.ssid);
        s.ssid.assign(ssid, strnlen(ssid, sizeof(cfg.sta.ssid)));
    }
    s.provisioned = !s.ssid.empty();
    s.stopped = s_stopped;
    s.connected = got_ip();
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
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    // Atomics plus a short s_ctrl_mu hold; never the radio mutex, so it is safe
    // from an ApObserver and while a scan runs.
    uint64_t up_at;
    {
        Lock lk(s_ctrl_mu);
        s.ap.enabled = s_ap.enabled;
        s.ap.open = s_ap.password.empty();
        s.ap.delay_s = s_ap.delay_s;
        s.ap.idle_s = s_ap.idle_s;
        up_at = s_up_at_ms;
    }
    s.ap.active = s_ap_mode;
    s.ap.ever_started = (xEventGroupGetBits(s_events) & kApStarted) != 0;
    s.ap.sta_paused = s_paused;
    s.ap.clients = s_ap_clients;
    s.ap.fault = s_ap_fault;
    if (up_at != ap::kNever && !s.ap.active && !s.stopped) {
        const uint64_t now = now_ms();
        s.ap.up_in_s = up_at > now ? static_cast<int64_t>((up_at - now + 999) / 1000) : 0;
    }
#endif
    return s;
}

bool set_credentials(const std::string& ssid, const std::string& password, bool user_join) {
    // A NUL would cut the SSID short in the driver and join a different network.
    if (!s_init || ssid.empty() || ssid.size() > 32 || password.size() > 64 ||
        ssid.find('\0') != std::string::npos) {
        return false;
    }
    wifi_config_t cfg{};
    std::memcpy(cfg.sta.ssid, ssid.data(), ssid.size());
    std::memcpy(cfg.sta.password, password.data(), password.size());
    // Refuse to fall back to an open network when a password was given.
    cfg.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;  // unused while WPA3 is off (sdkconfig.defaults)
    esp_err_t err;
    {
        Lock radio(s_wifi_mu);  // a running scan finishes first
        s_want_connect = false;
        local_disconnect();
        err = set_sta_config(cfg);
    }
    s_want_connect = true;  // on failure the previous network stays and is retried
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        return false;
    }
    s_backoff_ms = kBackoffMinMs;
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    // A user's join is never paused: the 65 s join window (§9.5). Putting the
    // previous network back after a failed attempt isn't one: it ends the
    // attempt's window, and the AP controller (kick) connects or, with a phone
    // on the AP, keeps the strict pause. connect_now() here would still see
    // the pause as it was during the attempt.
    {
        Lock lk(s_ctrl_mu);
        if (user_join) s_ctrl.explicit_join(now_ms());
        else s_ctrl.end_join();
    }
    if (user_join || s_ap_eval == nullptr) connect_now();  // no controller timer: nothing else would
#else
    (void)user_join;
    connect_now();
#endif
    kick();
    return true;
}

bool forget() {
    if (!s_init) return false;
    esp_err_t err;
    {
        Lock radio(s_wifi_mu);
        s_want_connect = false;
        local_disconnect();
        wifi_config_t cfg{};
        err = set_sta_config(cfg);
    }
    s_want_connect = true;  // a later set_credentials() connects again
    kick();                 // with AP mode on, the AP comes up at once (no network stored, §9.5)
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

bool scan(std::vector<ScanResult>& out, size_t max, ScanRefused* why) {
    ScanRefused unused;
    ScanRefused& refused = why != nullptr ? *why : unused;
    refused = ScanRefused::None;
    out.clear();
    if (!s_init) {
        refused = ScanRefused::Driver;
        return false;
    }
    if (s_stopped) {
        refused = ScanRefused::Stopped;
        return false;
    }
    // A scan would get in the way of a join in flight or of a stored network's
    // retries; allowed with nothing stored and while retries are paused, so a
    // phone on the AP can pick a network (§9.5).
    if (s_link == kConnecting || (provisioned() && s_link != kAssociated && !s_paused)) {
        refused = ScanRefused::Joining;
        return false;
    }
    std::unique_lock<std::mutex> radio(s_wifi_mu);  // blocking is fine: api_slow or improv task
    if (s_stopped) {  // stop() won the radio while we waited
        refused = ScanRefused::Stopped;
        return false;
    }
    const esp_err_t err = esp_wifi_scan_start(nullptr, true);  // IDF default scan config
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan: %s", esp_err_to_name(err));
        refused = ScanRefused::Driver;
        return false;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    n = std::min<uint16_t>(n, 32);
    std::vector<wifi_ap_record_t> recs(n);
    if (n > 0 && esp_wifi_scan_get_ap_records(&n, recs.data()) != ESP_OK) n = 0;
    if (n == 0) esp_wifi_clear_ap_list();  // frees the driver's list when nothing was read
    radio.unlock();
    recs.resize(n);
    std::sort(recs.begin(), recs.end(),
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });
    // Strongest first, so the first record of each SSID is its best.
    for (const wifi_ap_record_t& r : recs) {
        const auto* name = reinterpret_cast<const char*>(r.ssid);
        const std::string ssid(name, strnlen(name, sizeof(r.ssid)));
        if (ssid.empty()) continue;
        bool seen = false;
        for (const ScanResult& e : out) seen = seen || e.ssid == ssid;
        if (seen) continue;
        out.push_back({ssid, r.rssi, r.authmode != WIFI_AUTH_OPEN});
        if (out.size() >= max) break;
    }
    return true;
}

bool wait_connected(uint32_t timeout_ms) {
    if (!s_init) return false;
    return (xEventGroupWaitBits(s_events, kGotIp, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & kGotIp) != 0;
}

bool stop() {
    if (!s_init) return false;
    const bool had_ip = got_ip();
    s_stopped = true;
    std::unique_lock<std::mutex> radio(s_wifi_mu);  // a scan or an AP change finishes first
    esp_timer_stop(s_retry);
    xEventGroupClearBits(s_events, kGotIp);
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    // The AP and its phones go down with the station (§7.1); settings changed
    // from now on wait for restore().
    esp_timer_stop(s_ap_eval);
    {
        Lock lk(s_ctrl_mu);
        s_ctrl.stopped(had_ip);
        s_up_at_ms = ap::kNever;
    }
    s_paused = false;
    s_ap_clients = 0;
    sync_dns(false);
    const bool was_on = s_ap_mode.exchange(false);
#else
    (void)had_ip;
    const bool was_on = false;
#endif
    const esp_err_t err = esp_wifi_stop();
    s_link = kIdle;
    radio.unlock();
    if (err != ESP_OK) ESP_LOGE(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "WiFi%s stopped for the BLE transfer", was_on ? " (and the access point)" : "");
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    if (was_on) notify(ApEvent::Down);
#endif
    return err == ESP_OK;
}

bool restore(uint32_t timeout_ms) {
    if (!s_init) return false;
    esp_err_t err;
    {
        Lock radio(s_wifi_mu);
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
        reconcile_ap();
#endif
        s_backoff_ms = kBackoffMinMs;
        s_stopped = false;
        err = esp_wifi_start();  // STA_START -> connect (and AP_START when the AP is on)
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return false;
    }
    // STA_START's connect may have found the radio mutex still held; this one
    // doesn't (a no-op if that one got through).
    connect_now();
    kick();
    ESP_LOGI(TAG, "WiFi restarted");
    return wait_connected(timeout_ms);
}

bool set_ap(const ApSettings& s) {
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    if (!ap::validate_ap_password(s.password).empty() || !ap::validate_ap_delay(s.delay_s).empty() ||
        !ap::validate_ap_idle(s.idle_s)) {
        return false;
    }
    {
        Lock lk(s_ctrl_mu);
        s_ap = s;
        s_ap_pending = true;
        // Applied kApplyGraceMs later, so the reply to a phone on the AP goes
        // out before the radio changes (§9.5). The controller holds them back
        // itself; while stopped they wait for restore().
        s_apply_at_ms = now_ms() + ap::kApplyGraceMs;
    }
    kick();
    return true;
#else
    (void)s;
    return false;
#endif
}

bool factory_reset() {
    if (!s_init) return false;
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    {
        Lock lk(s_ctrl_mu);
        s_ap.enabled = false;
        s_ap_live.enabled = false;
        s_ap_pending = false;
    }
#endif
    esp_err_t err;
    {
        Lock radio(s_wifi_mu);
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
        // Down now, so no AP springs up before the reboot (§9.3, §9.5).
        if (mode_has_ap()) esp_wifi_set_mode(WIFI_MODE_STA);
        sync_dns(false);
#endif
        s_want_connect = false;  // the reboot follows
        local_disconnect();
        wifi_config_t cfg{};
        err = set_sta_config(cfg);
    }
    kick();
    return err == ESP_OK;
}

bool is_ap_socket(int sockfd) {
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    if (getsockname(sockfd, reinterpret_cast<sockaddr*>(&a), &len) != 0 || a.sin_family != AF_INET) return false;
    return std::memcmp(&a.sin_addr.s_addr, ap::kIp, sizeof(ap::kIp)) == 0;  // network order: byte for byte
#else
    (void)sockfd;
    return false;
#endif
}

void set_ap_observer(ApObserver fn) {
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    s_observer = fn;
#else
    (void)fn;
#endif
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
