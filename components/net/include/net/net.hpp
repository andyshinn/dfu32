// SPDX-License-Identifier: MIT
//
// Network manager (SPEC.md §7.1, §7.3, §9): WiFi station with credentials in
// the IDF WiFi NVS storage, automatic reconnect with backoff, mDNS hostname
// and SNTP. stop()/restore() are the radio-handover hooks for the DFU job.
#pragma once

#include <cstdint>
#include <string>

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

/// Like set_credentials(), but the new network is kept in RAM only: a power
/// cut during the attempt leaves the stored one as it was. Make it permanent
/// with commit_credentials() once it has connected (Improv, SPEC.md §9.2).
bool try_credentials(const std::string& ssid, const std::string& password);
/// Writes the network in use to flash. Call while connected (not connecting).
bool commit_credentials();

/// The network in use (stored, or being tried), so a failed Improv attempt
/// can put the previous one back. False when none is set.
bool stored_credentials(std::string& ssid, std::string& password);

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
