// SPDX-License-Identifier: MIT
//
// Improv Serial (SPEC.md §9.2): the Improv core on the console port. The
// console reader (main/console_io.cpp) splits frames out of the input and
// hands them here; one task owns the core and the connection attempt, and
// answers through the reader's write function. Unlike Improv BLE it is always
// on and needs no BLE lease, so it stays available after provisioning.
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "attempt.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
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
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(250);

struct Msg {
    bool invalid = false;       // the reader dropped a frame
    uint8_t type = 0;
    std::vector<uint8_t> data;  // may carry a passphrase
    ~Msg() { wipe(data); }
};

QueueHandle_t g_queue = nullptr;
SerialWriteFn g_write = nullptr;

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
    bool online_url(std::string& url) override {
        const net::Status st = net::status();
        if (!st.connected || st.ip.empty()) return false;
        url = "http://" + st.ip + "/";
        return true;
    }
    std::vector<Network> scan_networks() override;
};

SerialHost g_host;
Core g_core(g_host, Transport::Serial);
Attempt g_attempt;

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

void finish_connect(bool ok) {
    if (!ok) {
        g_attempt.give_up();
        g_core.connect_failed();
        return;
    }
    g_core.connect_succeeded(g_attempt.keep());
    // Improv BLE may still be open (it opens by itself while no network is
    // stored) and holds the BLE lease while it is: close it, or a flash would
    // be refused until someone ran `improv stop`. close() only posts to the
    // BLE task; its do_close() would end a *BLE* attempt as failed, but none
    // can be running: Attempt allows one at a time across both transports,
    // and this one has just ended.
    if (improv::active()) improv::close();
}

void handle(const Msg& m) {
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
        const TickType_t wait = g_attempt.active() ? kPollTicks : portMAX_DELAY;
        if (xQueueReceive(g_queue, &raw, wait) == pdTRUE && raw != nullptr) {
            std::unique_ptr<Msg> m(raw);
            handle(*m);
        }
        switch (g_attempt.poll()) {
            case Attempt::Poll::Joined: finish_connect(true); break;
            case Attempt::Poll::TimedOut: finish_connect(false); break;
            case Attempt::Poll::Idle:
            case Attempt::Poll::Pending: break;
        }
    }
}

void post(std::unique_ptr<Msg> m) {
    Msg* raw = m.release();
    if (g_queue == nullptr || xQueueSend(g_queue, &raw, 0) != pdTRUE) {
        ESP_LOGW(TAG, "serial: busy; dropping a frame");
        delete raw;
    }
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
    if (xTaskCreate(&serial_task, "improv_ser", 6144, nullptr, 4, nullptr) != pdPASS) {
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

}  // namespace dfu32::improv
