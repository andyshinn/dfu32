// SPDX-License-Identifier: MIT
//
// One WiFi connection attempt from Improv (SPEC.md §9.2), shared by the BLE
// and Serial transports: only one attempt runs at a time, whichever started it.
// The new network is saved to flash as it is applied; if it doesn't get an IP
// in time, the previous network is put back (and saved), or cleared if there
// was none. Not thread-safe: each
// transport calls its own Attempt from its own task.
#pragma once

#include <cstdint>
#include <string>

namespace dfu32::improv {

class Attempt {
public:
    enum class Poll : uint8_t { Idle, Pending, Joined, TimedOut };

    /// Starts connecting. False if another transport's attempt is running,
    /// something other than Improv holds the BLE lease (a flash: the radio
    /// may be handed over), or the settings can't be applied.
    bool begin(const std::string& ssid, const std::string& password, int64_t timeout_us);

    /// Joined once the device has an IP, TimedOut past the deadline.
    Poll poll() const;

    /// Keeps the new network (already saved). Returns the redirect URL.
    std::string keep();
    /// Puts the previous network back (or clears it again if there was none).
    void give_up();

    bool active() const noexcept { return active_; }
    /// An attempt from either transport is running.
    static bool any_running();
    const std::string& ssid() const noexcept { return trying_ssid_; }

private:
    void end();

    bool active_ = false;
    int64_t deadline_us_ = 0;
    bool had_previous_ = false;
    std::string previous_ssid_, previous_password_, trying_ssid_;
};

/// Improv BLE's close releases its lease through this: if a Serial attempt is
/// running on that lease, the attempt keeps it (and releases it when it ends).
void release_ble_lease();

}  // namespace dfu32::improv
