// SPDX-License-Identifier: MIT
//
// Status LEDs and the user button (SPEC.md §9.3, §13.4): the NeoPixel and the
// red attention LED through espressif/led_indicator (on led_strip), the
// button through espressif/button. What they show and what a hold does is
// decided in logic.hpp; this file is the driver and a 200 ms polling task.
#pragma once

#include <functional>

namespace dfu32::indicator {

struct Hooks {
    std::function<bool()> improv_active;
    std::function<bool()> identifying;
    // Button actions; they run on the indicator task, never in the button ISR/timer.
    std::function<void()> on_improv_window;  // released after 2-5 s
    std::function<void()> on_unclaim;        // released after 5-15 s
    std::function<void()> on_factory_reset;  // released after 15 s or more
};

/// Sets up whatever this board has (CONFIG_DFU32_NEOPIXEL_GPIO,
/// CONFIG_DFU32_STATUS_LED_GPIO, CONFIG_DFU32_BUTTON_GPIO; -1 = none) and
/// starts the task. Missing hardware is skipped, not an error.
bool start(const Hooks& hooks);

}  // namespace dfu32::indicator
