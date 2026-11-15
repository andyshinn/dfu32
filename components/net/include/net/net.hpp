// SPDX-License-Identifier: MIT
//
// Network manager (SPEC.md §7.1, §7.3, §9): WiFi station with credentials in
// the IDF WiFi NVS storage, automatic reconnect with backoff, mDNS hostname
// and SNTP, plus the optional SoftAP fallback with its captive DNS
// (SPEC.md §9.5). stop()/restore() are the radio-handover hooks for the DFU
// job; they take the AP down and back up with the station.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dfu32::net {

/// The SoftAP fallback (SPEC.md §9.5). All false/zero in a build without
/// CONFIG_ESP_WIFI_SOFTAP_SUPPORT (`supported` tells them apart).
struct ApStatus {
    bool supported = false;      // CONFIG_ESP_WIFI_SOFTAP_SUPPORT in this build
    bool enabled = false;        // setting in force (false after a bad stored password)
    bool active = false;         // driver mode includes the AP
    bool open = true;            // no password
    bool ever_started = false;   // WIFI_EVENT_AP_START seen since boot (OTA confirm, §12.3)
    bool sta_paused = false;     // automatic station retries paused for AP clients
    uint8_t clients = 0;
    uint32_t delay_s = 120;      // = ap::kDefaultDelayS (net's header can't see dfu32_core)
    int64_t up_in_s = -1;        // standby countdown, -1 = none pending
    const char* fault = nullptr; // "netif" | "start" | nullptr
};

struct Status {
    bool initialised = false;
    bool provisioned = false;   // an SSID is stored
    bool stopped = false;       // stop() called (radio handover), not yet restored
    bool connected = false;     // the station has an IPv4 address (the AP doesn't count)
    std::string ssid;
    std::string ip;             // station dotted quad, empty when not connected
    int rssi = 0;               // dBm, 0 when not connected
    uint8_t last_disconnect_reason = 0;  // wifi_err_reason_t of the last drop
    uint32_t reconnects = 0;
    std::string hostname;       // also the mDNS name (<hostname>.local) and the AP SSID
    bool time_synced = false;   // SNTP has set the clock
    ApStatus ap;
};

/// AP settings as stored in NVS (config, `api::DeviceConfig`). The caller
/// validates and persists them; net only holds them in RAM.
struct ApSettings {
    bool enabled = false;
    std::string password;       // "" = open
    uint32_t delay_s = 120;     // = ap::kDefaultDelayS
};

/// Brings up esp_netif, WiFi (STA), mDNS and SNTP, and loads the AP config
/// into the driver before esp_wifi_start() so a later AP start never beacons
/// the driver's default network. Connects if credentials are stored. Safe to
/// call once; returns false if the WiFi driver can't start. Never false
/// because of the AP: a failing init would turn the API off and roll back a
/// pending OTA (§12.3), so AP faults only show in `Status::ap.fault`.
bool init(const ApSettings& ap = {});

/// Never takes the radio mutex, so it is safe from any task (and from an
/// ApObserver).
Status status();

/// Stores credentials (persisted by the WiFi driver) and reconnects with them.
/// Opens the 65 s join window: retries run even while phones are on the AP.
bool set_credentials(const std::string& ssid, const std::string& password);

/// Clears stored credentials and disconnects. With AP mode on, the AP comes
/// up at once (no network stored, §9.5).
bool forget();

/// The network in use, so a failed Improv attempt can put the previous one
/// back. False when none is set.
bool stored_credentials(std::string& ssid, std::string& password);

struct ScanResult {
    std::string ssid;
    int rssi = 0;          // dBm
    bool secured = false;  // anything but an open network
};
/// Why scan() said no (POST /api/v1/wifi/scan turns it into the 409 detail).
enum class ScanRefused : uint8_t {
    None,
    Stopped,   // radio handed over for a transfer
    Joining,   // a connect is in flight, or a stored network is being retried
    Driver,    // esp_wifi_scan_start failed
};
/// Active WiFi scan (blocks for about 2 s, more on the C5): one entry per
/// SSID at its strongest, strongest first, hidden networks left out, at most
/// `max`. False (nothing scanned, `*why` set) while stopped for a transfer,
/// while a connect is in flight, or while a stored network is neither
/// associated nor paused for AP clients: a scan would get in the way of that
/// join. Allowed with nothing stored and while retries are paused, so a phone
/// on the AP can pick a network (§9.5).
bool scan(std::vector<ScanResult>& out, size_t max = 20, ScanRefused* why = nullptr);

/// Blocks until connected or `timeout_ms` passes.
bool wait_connected(uint32_t timeout_ms);

/// Radio handover: stop WiFi entirely (esp_wifi_stop) so BLE has the radio.
/// This also takes the AP and its phones down; AP settings changed while
/// stopped are kept and apply at restore().
bool stop();

/// Undo stop(): reconciles the AP mode and config first (so a disabled AP
/// never comes back, even briefly), then esp_wifi_start() and reconnect.
/// Opens the 65 s join window, so a phone that rejoined the AP doesn't pause
/// the §7.3 verification. Returns once connected or after `timeout_ms`
/// (false); reconnecting continues in the background.
bool restore(uint32_t timeout_ms);

/// Applies AP settings at runtime, kApplyGraceMs (1.5 s) later so the HTTP or
/// console reply goes out before the radio changes. Validated again with
/// dfu32_core; the caller persists them. While stopped they wait for
/// restore(). A password change while the AP is up deauthenticates every
/// client. False (nothing changed) if the settings don't validate.
bool set_ap(const ApSettings& s);

/// Factory reset (§9.3): AP mode off in RAM, AP down, station forgotten
/// (driver NVS). Call before config::erase_all(), so no AP comes up before
/// the reboot.
bool factory_reset();

/// True when the socket's local IPv4 address is the AP address, i.e. the
/// request arrived over the AP (captive-portal handling, §9.5).
bool is_ap_socket(int sockfd);

enum class ApEvent : uint8_t { Up, Down, ClientJoined };
using ApObserver = void (*)(ApEvent);
/// One observer (main: the open-AP warning, closing Improv's boot window).
/// Called on the esp_timer task (Down also on stop()'s caller), outside net's
/// locks; must not block (may call status()).
void set_ap_observer(ApObserver fn);

/// Unix seconds once SNTP has synced, else 0.
uint64_t unix_time();

/// Announces the HTTP API over mDNS (_http._tcp on `port`, path=/api/v1/).
void advertise_http(uint16_t port);

}  // namespace dfu32::net
