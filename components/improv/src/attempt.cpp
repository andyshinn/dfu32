// SPDX-License-Identifier: MIT
#include "attempt.hpp"

#include <atomic>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "improv/core.hpp"
#include "job/job.hpp"
#include "net/net.hpp"

namespace dfu32::improv {
namespace {

constexpr const char* TAG = "improv";
constexpr const char* kLeaseName = "improv-serial";

std::atomic<bool> g_running{false};  // an attempt from either transport

}  // namespace

bool Attempt::begin(const std::string& ssid, const std::string& password, int64_t timeout_us) {
    if (active_) return false;
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "WiFi settings refused: another Improv client is connecting");
        return false;
    }
    // Improv BLE holds the lease while it is open; otherwise take it for the
    // attempt, so no flash can stop WiFi underneath it.
    const char* holder = job::holder();
    if (holder != nullptr && std::strcmp(holder, "improv") != 0) {
        ESP_LOGW(TAG, "WiFi settings refused: %s is using the radio", holder);
        g_running = false;
        return false;
    }
    owns_lease_ = holder == nullptr && job::try_acquire(kLeaseName);
    if (holder == nullptr && !owns_lease_) {
        ESP_LOGW(TAG, "WiFi settings refused: a job just started");
        g_running = false;
        return false;
    }
    had_previous_ = net::stored_credentials(previous_ssid_, previous_password_);
    // RAM only until it connects: a power cut mid-attempt keeps the old network.
    if (!net::try_credentials(ssid, password)) {
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
    if (!net::commit_credentials()) ESP_LOGW(TAG, "connected, but saving the network failed; it is lost on reboot");
    wipe(previous_password_);
    const std::string url = "http://" + net::status().ip + "/";
    ESP_LOGI(TAG, "joined \"%s\"; provisioned, redirect %s", trying_ssid_.c_str(), url.c_str());
    end();
    return url;
}

void Attempt::give_up() {
    ESP_LOGW(TAG, "could not join \"%s\"; %s", trying_ssid_.c_str(),
             had_previous_ ? "going back to the previous network" : "clearing it again");
    if (had_previous_) {
        net::set_credentials(previous_ssid_, previous_password_);
    } else {
        net::forget();
    }
    wipe(previous_password_);
    end();
}

void Attempt::end() {
    active_ = false;
    if (owns_lease_) job::release();
    owns_lease_ = false;
    g_running = false;
}

}  // namespace dfu32::improv
