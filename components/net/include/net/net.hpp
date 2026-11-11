// SPDX-License-Identifier: MIT
//
// Network manager (SPEC.md §7.1, §7.3, §9): WiFi station with credentials in
// the IDF WiFi NVS storage, automatic reconnect with backoff, mDNS hostname
// and SNTP. stop()/restore() are the radio-handover hooks for the DFU job.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dfu32::net {

struct Status {
    bool initialised = false;
    bool provisioned = false;   // an SSID is stored
    bool stopped = false;       // stop() called (radio handover), not yet restored
    bool connected = false;     // has an IPv4 address
    std::string ssid;
    std::string ip;             // dotted quad, empty when not connected
    int rssi = 0;               // dBm, 0 when not connected
    uint8_t last_disconnect_reason = 0;  // wifi_err_reason_t of the last drop
    uint32_t reconnects = 0;
    std::string hostname;       // also the mDNS name (<hostname>.local)
    bool time_synced = false;   // SNTP has set the clock
};

/// Brings up esp_netif, WiFi (STA), mDNS and SNTP. Connects if credentials are
/// stored. Safe to call once; returns false if the WiFi driver can't start.
bool init();

Status status();

/// Stores credentials (persisted by the WiFi driver) and reconnects with them.
bool set_credentials(const std::string& ssid, const std::string& password);

/// Clears stored credentials and disconnects.
bool forget();

/// The network in use, so a failed Improv attempt can put the previous one
/// back. False when none is set.
bool stored_credentials(std::string& ssid, std::string& password);

struct ScanResult {
    std::string ssid;
    int rssi = 0;          // dBm
    bool secured = false;  // anything but an open network
};
/// Active WiFi scan (blocks for about 2 s): one entry per SSID at its
/// strongest, strongest first, hidden networks left out, at most `max`.
/// False (and nothing scanned) while stopped for a transfer, or while a
/// stored network is being joined: a scan would get in the way of that.
bool scan(std::vector<ScanResult>& out, size_t max = 20);

/// Blocks until connected or `timeout_ms` passes.
bool wait_connected(uint32_t timeout_ms);

/// Radio handover: stop WiFi entirely (esp_wifi_stop) so BLE has the radio.
bool stop();

/// Undo stop(): esp_wifi_start() and reconnect. Returns once connected or
/// after `timeout_ms` (false); reconnecting continues in the background.
bool restore(uint32_t timeout_ms);

/// Unix seconds once SNTP has synced, else 0.
uint64_t unix_time();

/// Announces the HTTP API over mDNS (_http._tcp on `port`, path=/api/v1/).
void advertise_http(uint16_t port);

}  // namespace dfu32::net
