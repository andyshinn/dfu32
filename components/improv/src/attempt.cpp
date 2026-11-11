// SPDX-License-Identifier: MIT
#include "attempt.hpp"

#include <cstring>
#include <mutex>

#include "esp_log.h"
#include "esp_timer.h"
#include "improv/core.hpp"
#include "job/job.hpp"
#include "net/net.hpp"

namespace dfu32::improv {
namespace {

constexpr const char* TAG = "improv";
constexpr const char* kLeaseName = "improv-serial";

// One attempt at a time, from either transport, and whether it holds the BLE
// lease: its own ("improv-serial"), or Improv BLE's, adopted when the BLE
// window closed under it (release_ble_lease()).
std::mutex g_mu;
bool g_running = false;     // guarded by g_mu
bool g_owns_lease = false;  // guarded by g_mu

}  // namespace

bool Attempt::any_running() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_running;
}

void release_ble_lease() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_running && !g_owns_lease) {
        // A Serial attempt relied on the BLE window's lease: keep it held for
        // the attempt (it releases it when it ends), or a flash could start
        // and stop WiFi underneath it.
        g_owns_lease = true;
        ESP_LOGI(TAG, "Improv BLE closed during a WiFi attempt; the attempt keeps the BLE lease");
        return;
    }
    job::release();
}

bool Attempt::begin(const std::string& ssid, const std::string& password, int64_t timeout_us) {
    if (active_) return false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_running) {
            ESP_LOGW(TAG, "WiFi settings refused: another Improv client is connecting");
            return false;
        }
        // Improv BLE holds the lease while it is open (and hands it over if it
        // closes mid-attempt); otherwise take it for the attempt, so no flash
        // can stop WiFi underneath it.
        const char* holder = job::holder();
        if (holder != nullptr && std::strcmp(holder, "improv") != 0) {
            ESP_LOGW(TAG, "WiFi settings refused: %s is using the radio", holder);
            return false;
        }
        g_owns_lease = holder == nullptr && job::try_acquire(kLeaseName);
        if (holder == nullptr && !g_owns_lease) {
            ESP_LOGW(TAG, "WiFi settings refused: a job just started");
            return false;
        }
        g_running = true;
    }
    had_previous_ = net::stored_credentials(previous_ssid_, previous_password_);
    // Saved to flash now, as the console's `wifi` command does; a failed
    // attempt puts the previous network back (give_up). An earlier version
    // tried the network from RAM (esp_wifi_set_storage(RAM)) and re-applied
    // the same config with FLASH storage once it joined, but the driver
    // doesn't write an unchanged config, so the network was lost on reboot.
    if (!net::set_credentials(ssid, password)) {
        ESP_LOGE(TAG, "could not apply the WiFi settings");
        wipe(previous_password_);
        end();
        return false;
    }
    active_ = true;
    trying_ssid_ = ssid;
    deadline_us_ = esp_timer_get_time() + timeout_us;
    ESP_LOGI(TAG, "WiFi settings received for \"%s\"; connecting", ssid.c_str());
    return true;
}

Attempt::Poll Attempt::poll() const {
    if (!active_) return Poll::Idle;
    if (net::status().connected) return Poll::Joined;
    return esp_timer_get_time() >= deadline_us_ ? Poll::TimedOut : Poll::Pending;
}

std::string Attempt::keep() {
    wipe(previous_password_);  // the new network is already saved (begin)
    const std::string url = "http://" + net::status().ip + "/";
    ESP_LOGI(TAG, "joined \"%s\"; provisioned, redirect %s", trying_ssid_.c_str(), url.c_str());
    end();
    return url;
}

void Attempt::give_up() {
    ESP_LOGW(TAG, "could not join \"%s\"; %s", trying_ssid_.c_str(),
             had_previous_ ? "going back to the previous network" : "clearing it again");
    // The new network was saved by begin(), so this restore is all that keeps
    // the previous one: say so if it fails.
    const bool restored = had_previous_ ? net::set_credentials(previous_ssid_, previous_password_) : net::forget();
    if (!restored) {
        ESP_LOGE(TAG, "%s failed: \"%s\" is still the saved network; set WiFi again (Improv or `wifi`)",
                 had_previous_ ? "restoring the previous network" : "clearing the network", trying_ssid_.c_str());
    }
    wipe(previous_password_);
    end();
}

void Attempt::end() {
    active_ = false;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_owns_lease) job::release();
    g_owns_lease = false;
    g_running = false;
}

}  // namespace dfu32::improv
