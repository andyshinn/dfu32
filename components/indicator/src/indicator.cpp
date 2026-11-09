// SPDX-License-Identifier: MIT
#include "indicator/indicator.hpp"

#include <atomic>

#include "button_gpio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "indicator/logic.hpp"
#include "iot_button.h"
#include "job/job.hpp"
#include "led_indicator.h"
#include "led_indicator_gpio.h"
#include "led_indicator_strips.h"
#include "net/net.hpp"
#include "sdkconfig.h"

namespace dfu32::indicator {
namespace {

constexpr const char* TAG = "indicator";
constexpr uint32_t kPollMs = 200;

// ---- NeoPixel blink lists (index = Pattern) --------------------------------------
// An RGB step sets the colour; BRIGHTNESS / BREATHE steps then scale it (the
// strip driver keeps hue and saturation and replaces the value).

// The index bits (MAX_INDEX = all LEDs) overflow int in these C macros: cast.
#define ALL(v) static_cast<uint32_t>(INSERT_INDEX(MAX_INDEX, (v)))
#define COLOR(r, g, b) {LED_BLINK_RGB, static_cast<uint32_t>(SET_IRGB(MAX_INDEX, (r), (g), (b))), 0}
#define LEVEL(v, ms) {LED_BLINK_BRIGHTNESS, ALL(v), (ms)}
#define FADE(v, ms) {LED_BLINK_BREATHE, ALL(v), (ms)}
#define LOOP {LED_BLINK_LOOP, 0, 0}
#define STOP {LED_BLINK_STOP, 0, 0}

const blink_step_t kOff[] = {COLOR(0, 0, 0), LEVEL(0, 0), STOP};
const blink_step_t kIdle[] = {COLOR(0, 255, 0), LEVEL(40, 0), STOP};  // after gamma: dim
const blink_step_t kOffline[] = {COLOR(255, 160, 0), LEVEL(80, 1000), LEVEL(0, 1000), LOOP};
const blink_step_t kImprov[] = {COLOR(255, 255, 255), LEVEL(120, 400), LEVEL(0, 1600), LOOP};
const blink_step_t kFetching[] = {COLOR(0, 0, 255), FADE(255, 1000), FADE(20, 1000), LOOP};
const blink_step_t kAwaiting[] = {COLOR(0, 255, 255), LEVEL(200, 500), LEVEL(0, 500), LOOP};
const blink_step_t kTransfer[] = {COLOR(255, 0, 255), LEVEL(40, 0), STOP};  // brightness set from progress
const blink_step_t kSuccess[] = {COLOR(0, 255, 0), LEVEL(255, 0), STOP};
const blink_step_t kFailed[] = {COLOR(255, 0, 0), LEVEL(255, 0), STOP};
const blink_step_t kErased[] = {COLOR(255, 0, 0), LEVEL(255, 100), LEVEL(0, 100), LOOP};
const blink_step_t kIdentify[] = {COLOR(255, 255, 255), LEVEL(255, 100), LEVEL(0, 100), LOOP};
const blink_step_t kHold2[] = {COLOR(255, 110, 0), LEVEL(255, 400), LEVEL(0, 400), LOOP};
const blink_step_t kHold5[] = {COLOR(255, 110, 0), LEVEL(255, 100), LEVEL(0, 100), LOOP};
const blink_step_t kHold15[] = {COLOR(255, 110, 0), LEVEL(255, 0), STOP};

const blink_step_t* kNeoLists[] = {kOff,    kIdle,    kOffline,  kImprov, kFetching, kAwaiting, kTransfer,
                                   kSuccess, kFailed, kErased, kIdentify, kHold2,   kHold5,    kHold15};
static_assert(sizeof(kNeoLists) / sizeof(kNeoLists[0]) == static_cast<size_t>(Pattern::Count));

// ---- red LED (plain GPIO) -----------------------------------------------------------
const blink_step_t kRedOff[] = {{LED_BLINK_HOLD, LED_STATE_OFF, 0}, STOP};
const blink_step_t kRedOn[] = {{LED_BLINK_HOLD, LED_STATE_ON, 0}, STOP};
const blink_step_t kRedBlink[] = {{LED_BLINK_HOLD, LED_STATE_ON, 250}, {LED_BLINK_HOLD, LED_STATE_OFF, 250}, LOOP};
const blink_step_t* kRedLists[] = {kRedOff, kRedOn, kRedBlink};  // index = RedLed

#undef ALL
#undef COLOR
#undef LEVEL
#undef FADE
#undef LOOP
#undef STOP

Hooks g_hooks;
led_indicator_handle_t g_neo = nullptr;
led_indicator_handle_t g_red = nullptr;
button_handle_t g_button = nullptr;
// One clock for both the hold feedback and the release action, so the LED
// never shows one level while the release triggers another.
std::atomic<int64_t> g_pressed_at_us{0};       // 0 = not pressed
QueueHandle_t g_actions = nullptr;             // ButtonAction, handled on the task in order

uint32_t uptime_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

bool setup_neopixel() {
#if CONFIG_DFU32_NEOPIXEL_GPIO >= 0
#if CONFIG_DFU32_NEOPIXEL_POWER_GPIO >= 0
    const auto pwr = static_cast<gpio_num_t>(CONFIG_DFU32_NEOPIXEL_POWER_GPIO);
    gpio_reset_pin(pwr);
    gpio_set_direction(pwr, GPIO_MODE_OUTPUT);
    gpio_set_level(pwr, 1);
#endif
    led_indicator_strips_config_t strips{};
    strips.led_strip_cfg.strip_gpio_num = CONFIG_DFU32_NEOPIXEL_GPIO;
    strips.led_strip_cfg.max_leds = 1;
    strips.led_strip_cfg.led_model = LED_MODEL_WS2812;
    // GRB, as LED_STRIP_COLOR_COMPONENT_FMT_GRB (a C compound literal) spells it.
    auto& fmt = strips.led_strip_cfg.color_component_format.format;
    fmt.r_pos = 1;
    fmt.g_pos = 0;
    fmt.b_pos = 2;
    fmt.w_pos = 3;
    fmt.bytes_per_color = 1;
    fmt.num_components = 3;
    strips.led_strip_driver = LED_STRIP_RMT;
    strips.led_strip_rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    strips.led_strip_rmt_cfg.resolution_hz = 10 * 1000 * 1000;
    led_indicator_config_t cfg{};
    cfg.blink_lists = kNeoLists;
    cfg.blink_list_num = static_cast<uint16_t>(Pattern::Count);
    const esp_err_t err = led_indicator_new_strips_device(&cfg, &strips, &g_neo);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NeoPixel on GPIO %d unavailable: %s", CONFIG_DFU32_NEOPIXEL_GPIO, esp_err_to_name(err));
        g_neo = nullptr;
        return false;
    }
#endif
    return true;
}

bool setup_red() {
#if CONFIG_DFU32_STATUS_LED_GPIO >= 0
    led_indicator_gpio_config_t gpio{};
#if CONFIG_DFU32_STATUS_LED_ACTIVE_LOW
    gpio.is_active_level_high = false;
#else
    gpio.is_active_level_high = true;
#endif
    gpio.gpio_num = CONFIG_DFU32_STATUS_LED_GPIO;
    led_indicator_config_t cfg{};
    cfg.blink_lists = kRedLists;
    cfg.blink_list_num = 3;
    const esp_err_t err = led_indicator_new_gpio_device(&cfg, &gpio, &g_red);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "status LED on GPIO %d unavailable: %s", CONFIG_DFU32_STATUS_LED_GPIO, esp_err_to_name(err));
        g_red = nullptr;
        return false;
    }
#endif
    return true;
}

void on_press_down(void*, void*) { g_pressed_at_us = esp_timer_get_time(); }

void on_press_up(void*, void*) {
    const int64_t at = g_pressed_at_us.exchange(0);
    if (at == 0) return;  // missed the press: do nothing rather than guess
    const auto held = static_cast<uint32_t>((esp_timer_get_time() - at) / 1000);
    const ButtonAction a = classify_hold(held);
    if (a != ButtonAction::None && xQueueSend(g_actions, &a, 0) != pdTRUE) {
        ESP_LOGW(TAG, "button action dropped (previous ones still running)");
    }
}

bool setup_button() {
#if CONFIG_DFU32_BUTTON_GPIO >= 0
    button_config_t cfg{};
    button_gpio_config_t gpio{};
    gpio.gpio_num = CONFIG_DFU32_BUTTON_GPIO;
    gpio.active_level = 0;  // to ground when pressed (BOOT / user buttons on the supported boards)
    // Input-only pads (GPIO34-39 on the classic ESP32) have no internal pull-up;
    // those boards pull the button up externally.
    gpio.disable_pull = !GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_DFU32_BUTTON_GPIO);
    esp_err_t err = iot_button_new_gpio_device(&cfg, &gpio, &g_button);
    if (err == ESP_OK) err = iot_button_register_cb(g_button, BUTTON_PRESS_DOWN, nullptr, &on_press_down, nullptr);
    if (err == ESP_OK) err = iot_button_register_cb(g_button, BUTTON_PRESS_UP, nullptr, &on_press_up, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "button on GPIO %d unavailable: %s", CONFIG_DFU32_BUTTON_GPIO, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "button on GPIO %d: hold 2 s = Improv BLE, 5 s = clear the API token, 15 s = factory reset",
             CONFIG_DFU32_BUTTON_GPIO);
#endif
    return true;
}

void run_action(ButtonAction a) {
    switch (a) {
        case ButtonAction::ImprovWindow:
            ESP_LOGI(TAG, "button: opening Improv BLE for 5 minutes");
            if (g_hooks.on_improv_window) g_hooks.on_improv_window();
            break;
        case ButtonAction::ClearToken:
            ESP_LOGW(TAG, "button: clearing the API token (API open)");
            if (g_hooks.on_clear_token) g_hooks.on_clear_token();
            break;
        case ButtonAction::FactoryReset:
            ESP_LOGW(TAG, "button: factory reset");
            if (g_hooks.on_factory_reset) g_hooks.on_factory_reset();
            break;
        case ButtonAction::None: break;
    }
}

void indicator_task(void*) {
    Tracker tracker;
    Output last{};
    bool first = true;
    for (;;) {
        ButtonAction a;
        while (xQueueReceive(g_actions, &a, 0) == pdTRUE) run_action(a);

        const job::Snapshot s = job::snapshot();
        const uint32_t now = uptime_s();
        tracker.update(s, now);
        const net::Status ns = net::status();
        Inputs in;
        in.job_state = s.state;
        in.bytes_done = s.bytes_done;
        in.bytes_total = s.bytes_total;
        in.erased_pending = tracker.erased_pending();
        in.failed = tracker.failed();
        in.success_showing = tracker.success_showing(now);
        in.provisioned = ns.provisioned;
        in.wifi_connected = ns.connected;
        in.wifi_stopped = ns.stopped;
        in.improv_active = g_hooks.improv_active && g_hooks.improv_active();
        in.identify = g_hooks.identifying && g_hooks.identifying();
        const int64_t pressed = g_pressed_at_us.load();
        in.hold_level = pressed ? hold_level(static_cast<uint32_t>((esp_timer_get_time() - pressed) / 1000)) : 0;

        const Output o = decide(in);
        if (g_neo != nullptr) {
            if (first || o.pattern != last.pattern) {
                if (!first) led_indicator_stop(g_neo, static_cast<int>(last.pattern));
                led_indicator_start(g_neo, static_cast<int>(o.pattern));
            }
            if (o.pattern == Pattern::Transfer && (first || o.brightness != last.brightness ||
                                                   last.pattern != Pattern::Transfer)) {
                led_indicator_set_brightness(g_neo, static_cast<uint32_t>(INSERT_INDEX(MAX_INDEX, o.brightness)));
            }
        }
        if (g_red != nullptr && (first || o.red != last.red)) {
            if (!first) led_indicator_stop(g_red, static_cast<int>(last.red));
            led_indicator_start(g_red, static_cast<int>(o.red));
        }
        last = o;
        first = false;
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

}  // namespace

bool start(const Hooks& hooks) {
    g_hooks = hooks;
    g_actions = xQueueCreate(4, sizeof(ButtonAction));
    if (g_actions == nullptr) return false;
    setup_neopixel();
    setup_red();
    setup_button();
    if (xTaskCreate(&indicator_task, "indicator", 4096, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "indicator task create failed");
        return false;
    }
    return true;
}

}  // namespace dfu32::indicator
