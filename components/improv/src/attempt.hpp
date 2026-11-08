// SPDX-License-Identifier: MIT
//
// One WiFi connection attempt from Improv (SPEC.md §9.2), shared by the BLE
// and Serial transports: only one attempt runs at a time, whichever started it.
// The new network is tried from RAM; it is written to flash only once it has
// an IP, otherwise the previous network is put back. Not thread-safe: each
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

    /// Keeps the new network (writes it to flash). Returns the redirect URL.
    std::string keep();
    /// Puts the previous network back (or clears it again if there was none).
    void give_up();

    bool active() const noexcept { return active_; }
    const std::string& ssid() const noexcept { return trying_ssid_; }

private:
    void end();

    bool active_ = false;
    bool owns_lease_ = false;
    int64_t deadline_us_ = 0;
    bool had_previous_ = false;
    std::string previous_ssid_, previous_password_, trying_ssid_;
};

}  // namespace dfu32::improv
