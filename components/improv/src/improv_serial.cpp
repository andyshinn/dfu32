// SPDX-License-Identifier: MIT
//
// Improv Serial (SPEC.md §9.2): the Improv core on the console port. The
// console reader (main/console_io.cpp) splits frames out of the input and
// hands them here; one task owns the core and the connection attempt, and
// answers through the reader's write function. Unlike Improv BLE it is always
// on and needs no BLE lease, so it stays available after provisioning.
// The same task runs the web UI's station setup (PUT /api/v1/wifi, SPEC.md
// §9.5), so the web is just a third source of the one shared attempt.
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "attempt.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "improv/improv.hpp"
#include "improv/serial_frame.hpp"
#include "job/job.hpp"
#include "net/net.hpp"
#include "sdkconfig.h"

namespace dfu32::improv {
namespace {

constexpr const char* TAG = "improv";
// Under the browser client's 30 s RPC timeout, so a failure is reported
// before the client gives up on its own.
constexpr int64_t kConnectWaitUs = 25LL * 1000000;
// The web attempt waits as long as Improv BLE's (§9.2, §9.5): no browser
// client with its own RPC timeout to stay under.
constexpr int64_t kWebConnectWaitUs = 30LL * 1000000;
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(250);

struct Msg {
    bool invalid = false;       // the reader dropped a frame
    bool web = false;           // a web_connect() request, not a frame
    uint8_t type = 0;
    std::vector<uint8_t> data;  // may carry a passphrase
    std::string ssid, password; // web only
    int64_t start_at_us = 0;    // web only
    ~Msg() {
        wipe(data);
        wipe(ssid);
        wipe(password);
    }
};

QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task = nullptr;
SerialWriteFn g_write = nullptr;

// The web request waiting for its start time (task-owned).
struct WebRequest {
    bool pending = false;
    int64_t start_at_us = 0;
    std::string ssid, password;
};
WebRequest g_web_req;

// The last web attempt's outcome: written by the task, read by the httpd task
// (/status, PUT /wifi), so behind a spinlock.
portMUX_TYPE g_web_mux = portMUX_INITIALIZER_UNLOCKED;
WebAttempt g_web;  // guarded by g_web_mux

void set_web(WebAttemptState state, uint8_t reason = 0, bool refused = false) {
    taskENTER_CRITICAL(&g_web_mux);
    g_web = {state, reason, refused};
    taskEXIT_CRITICAL(&g_web_mux);
}

void send(uint8_t type, std::span<const uint8_t> data) {
    const std::vector<uint8_t> frame = encode_serial_frame(type, data);
    if (!frame.empty() && g_write != nullptr) g_write(frame);
}

class SerialHost final : public IHost {
public:
    void publish_state(State s) override {
        const auto v = static_cast<uint8_t>(s);
        send(kSerialCurrentState, std::span<const uint8_t>(&v, 1));
    }
    void publish_error(Error e) override {
        const auto v = static_cast<uint8_t>(e);
        send(kSerialErrorState, std::span<const uint8_t>(&v, 1));
        if (e != Error::None) ESP_LOGW(TAG, "serial: error 0x%02x", v);
    }
    void publish_result(std::span<const uint8_t> packet) override { send(kSerialRpcResult, packet); }
    bool begin_connect(const std::string& ssid, const std::string& password) override;
    void identify() override {}  // not a Serial command
    DeviceInfo device_info() override {
        return {"DFU32", esp_app_get_description()->version, chip_name(CONFIG_IDF_TARGET), net::status().hostname,
            CONFIG_DFU32_BOARD_NAME};
    }
    bool provisioned(std::string& url) override {
        const net::Status st = net::status();
        if (!st.provisioned) return false;
        // The URL only once it is on the network: the installer shows its
        // "Visit Device" link exactly when one is given, so a device still
        // joining gets "Change Wi-Fi" without a link that wouldn't answer.
        if (st.connected && !st.ip.empty()) url = "http://" + st.ip + "/";
        return true;
    }
    std::vector<Network> scan_networks() override;
};

SerialHost g_host;
Core g_core(g_host, Transport::Serial);
Attempt g_attempt;
Attempt g_web_attempt{"wifi-setup"};

bool SerialHost::begin_connect(const std::string& ssid, const std::string& password) {
    return g_attempt.begin(ssid, password, kConnectWaitUs);
}

std::vector<Network> SerialHost::scan_networks() {
    // Not while a flash or console operation has the radio (a scan would slow
    // the BLE transfer down), nor while an Improv attempt from either
    // transport is joining a network (a scan would get in the way of
    // associating). The client then just sees no networks for now.
    // The scan blocks this task for about 2 s on a 2.4 GHz chip; frames that
    // arrive meanwhile wait in the queue (4 deep; the client sends one RPC at
    // a time). On a dual-band chip (ESP32-C5) net::scan() covers the 5 GHz
    // channels too, with a passive dwell on each DFS channel, so expect
    // several times that. Not yet measured on hardware (SPEC.md §4, [bench]).
    if (Attempt::any_running()) return {};
    const char* holder = job::holder();
    if (holder != nullptr && std::strcmp(holder, "improv") != 0) return {};
    std::vector<net::ScanResult> found;
    if (!net::scan(found)) return {};
    std::vector<Network> out;
    out.reserve(found.size());
    for (net::ScanResult& r : found) out.push_back({std::move(r.ssid), r.rssi, r.secured});
    return out;
}

void close_ble_window() {
    // Improv BLE may still be open (it opens by itself while no network is
    // stored) and holds the BLE lease while it is: close it, or a flash would
    // be refused until someone ran `improv stop`. close() only posts to the
    // BLE task; its do_close() would end a *BLE* attempt as failed, but none
    // can be running: Attempt allows one at a time across every source, and
    // this one has just ended.
    if (improv::active()) improv::close();
}

void finish_connect(bool ok) {
    if (!ok) {
        g_attempt.give_up();
        g_core.connect_failed();
        return;
    }
    g_core.connect_succeeded(g_attempt.keep());
    close_ble_window();
}

void start_web_attempt() {
    WebRequest& r = g_web_req;
    r.pending = false;
    ESP_LOGI(TAG, "WiFi setup from the web page: joining \"%s\"", r.ssid.c_str());
    const bool ok = g_web_attempt.begin(r.ssid, r.password, kWebConnectWaitUs);
    wipe(r.password);  // used (begin() refuses or has saved it)
    r.password.clear();
    if (!ok) {
        // Attempt::begin() logged why (another attempt, a flash, NVS).
        set_web(WebAttemptState::Failed, 0, true);
        return;
    }
    set_web(WebAttemptState::Joining);
}

void poll_web_attempt() {
    switch (g_web_attempt.poll()) {
        case Attempt::Poll::Joined:
            g_web_attempt.keep();
            set_web(WebAttemptState::Joined);
            ESP_LOGI(TAG, "WiFi setup from the web page: joined");
            close_ble_window();
            break;
        case Attempt::Poll::TimedOut: {
            const uint8_t reason = g_web_attempt.last_reason();
            g_web_attempt.give_up();  // logs the rollback
            set_web(WebAttemptState::Failed, reason);
            ESP_LOGW(TAG, "WiFi setup from the web page: failed (reason %u)", reason);
            break;
        }
        case Attempt::Poll::Idle:
        case Attempt::Poll::Pending: break;
    }
}

void handle(const Msg& m) {
    if (m.web) {
        // Copied, not moved: a moved short string leaves its bytes behind in
        // the message, where the destructor's wipe no longer sees them.
        g_web_req.pending = true;
        g_web_req.start_at_us = m.start_at_us;
        g_web_req.ssid = m.ssid;
        g_web_req.password = m.password;
        return;
    }
    if (m.invalid) {
        g_core.handle_rpc({});  // reports INVALID_RPC
        return;
    }
    if (m.type == kSerialRpc) g_core.handle_rpc(m.data);
    // Other types are device-to-client; ignore them if echoed back.
}

void serial_task(void*) {
    for (;;) {
        Msg* raw = nullptr;
        const bool busy = g_attempt.active() || g_web_attempt.active() || g_web_req.pending;
        if (xQueueReceive(g_queue, &raw, busy ? kPollTicks : portMAX_DELAY) == pdTRUE && raw != nullptr) {
            std::unique_ptr<Msg> m(raw);
            handle(*m);
        }
        switch (g_attempt.poll()) {
            case Attempt::Poll::Joined: finish_connect(true); break;
            case Attempt::Poll::TimedOut: finish_connect(false); break;
            case Attempt::Poll::Idle:
            case Attempt::Poll::Pending: break;
        }
        if (g_web_req.pending && esp_timer_get_time() >= g_web_req.start_at_us) start_web_attempt();
        poll_web_attempt();
    }
}

bool post(std::unique_ptr<Msg> m) {
    Msg* raw = m.release();
    if (g_queue == nullptr || xQueueSend(g_queue, &raw, 0) != pdTRUE) {
        ESP_LOGW(TAG, "serial: busy; dropping a %s", raw->web ? "web request" : "frame");
        delete raw;
        return false;
    }
    return true;
}

}  // namespace

bool serial_start(SerialWriteFn write) {
    if (g_queue != nullptr) return true;
    g_write = write;
    g_queue = xQueueCreate(4, sizeof(Msg*));
    if (g_queue == nullptr) return false;
    // Started before any frame can arrive, without announcing itself: a
    // terminal on the port shouldn't see a frame on every boot.
    g_core.start(false);
    if (xTaskCreate(&serial_task, "improv_ser", 6144, nullptr, 4, &g_task) != pdPASS) {
        ESP_LOGE(TAG, "Improv Serial task failed to start");
        return false;
    }
    return true;
}

void serial_deliver(uint8_t type, std::span<const uint8_t> data) {
    auto m = std::make_unique<Msg>();
    m->type = type;
    m->data.assign(data.begin(), data.end());
    post(std::move(m));
}

void serial_invalid_frame() {
    auto m = std::make_unique<Msg>();
    m->invalid = true;
    post(std::move(m));
}

bool web_connect(const std::string& ssid, const std::string& password, uint32_t delay_ms) {
    if (g_queue == nullptr || g_task == nullptr) return false;
    WebAttempt before;
    taskENTER_CRITICAL(&g_web_mux);
    before = g_web;
    const bool busy = before.state == WebAttemptState::Pending || before.state == WebAttemptState::Joining;
    // Pending from here, so a second PUT /wifi and attempt_running() see it
    // before the task has even taken the message.
    if (!busy) g_web = {WebAttemptState::Pending, 0, false};
    taskEXIT_CRITICAL(&g_web_mux);
    if (busy) return false;
    auto m = std::make_unique<Msg>();
    m->web = true;
    m->ssid = ssid;
    m->password = password;
    // Started from now, not from when the task takes the message: the delay
    // only has to let the 202 reach the phone (§9.5).
    m->start_at_us = esp_timer_get_time() + int64_t{delay_ms} * 1000;
    if (post(std::move(m))) return true;
    set_web(before.state, before.reason, before.refused);  // the last outcome stays readable
    return false;
}

WebAttempt web_attempt() {
    taskENTER_CRITICAL(&g_web_mux);
    const WebAttempt w = g_web;
    taskEXIT_CRITICAL(&g_web_mux);
    return w;
}

bool attempt_running() { return Attempt::any_running() || web_attempt().state == WebAttemptState::Pending; }

}  // namespace dfu32::improv
