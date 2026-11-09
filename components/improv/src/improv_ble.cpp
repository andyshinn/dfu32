// SPDX-License-Identifier: MIT
//
// One task owns all Improv BLE state: opening and closing, the core, the RPC
// buffer, the advert and the connection attempt. Everything else (console,
// button, NimBLE callbacks, timers) posts a message to it, so nothing here
// races. Other tasks read a small mirror (active, state) under a mutex.
#include "improv/improv.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "NimBLEDevice.h"
#include "attempt.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "improv.h"  // vendored improv-wifi/sdk-cpp: UUIDs, capabilities
#include "job/job.hpp"
#include "net/net.hpp"
#include "sdkconfig.h"

namespace dfu32::improv {
namespace {

constexpr const char* TAG = "improv";
constexpr int64_t kConnectWaitUs = 30LL * 1000000;          // per attempt
constexpr uint64_t kButtonWindowUs = 5ULL * 60 * 1000000;    // SPEC.md §9.2
constexpr int64_t kCloseAfterProvisionUs = 5LL * 1000000;    // let the client read the result
constexpr int64_t kIdentifyUs = 10LL * 1000000;
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(250);        // while connecting / closing soon
constexpr TickType_t kOpenReplyTicks = pdMS_TO_TICKS(2000);
constexpr uint16_t kServiceDataUuid = 0x4677;                // Improv BLE advertisement service data
constexpr uint8_t kCapabilities = ::improv::CAPABILITY_IDENTIFY;
#ifdef CONFIG_DFU32_IMPROV_BLE
constexpr bool kEnabled = true;
#else
constexpr bool kEnabled = false;
#endif

// ---- messages to the task ---------------------------------------------------------

enum class MsgKind : uint8_t { Open, Close, Write, ClientGone, WindowExpired };

struct Msg {
    MsgKind kind = MsgKind::Close;
    Window window = Window::Console;
    uint32_t gen = 0;                 // WindowExpired: which window it was armed for
    std::vector<uint8_t> data;        // Write
    SemaphoreHandle_t reply = nullptr;  // Open: given once `ok` is set
    bool* ok = nullptr;
    ~Msg() { wipe(data); }  // a WiFi-settings write carries the passphrase
};

QueueHandle_t g_queue = nullptr;

bool post(std::unique_ptr<Msg> m) {
    Msg* raw = m.release();
    if (g_queue == nullptr || xQueueSend(g_queue, &raw, 0) != pdTRUE) {
        ESP_LOGE(TAG, "queue full; dropping a request");
        delete raw;
        return false;
    }
    return true;
}

// ---- mirror for other tasks ----------------------------------------------------------

std::mutex g_mirror_mu;
Snapshot g_mirror;  // guarded by g_mirror_mu
std::atomic<int64_t> g_identify_until_us{0};
std::atomic<uint32_t> g_armed_gen{0};  // window generation the timer was last armed for

void set_mirror(bool active, State s) {
    std::lock_guard<std::mutex> lk(g_mirror_mu);
    g_mirror.active = active;
    g_mirror.state = s;
}

// ---- task-owned state -----------------------------------------------------------------

class Host final : public IHost {
public:
    void publish_state(State s) override;
    void publish_error(Error e) override;
    void publish_result(std::span<const uint8_t> packet) override;
    bool begin_connect(const std::string& ssid, const std::string& password) override;
    void identify() override;
    DeviceInfo device_info() override;
};

Host g_host;
Core g_core(g_host);
RpcAssembler g_rpc_buf;

struct Session {
    bool active = false;
    uint32_t window_gen = 0;      // bumped by every open/close/re-arm; stale timer messages are ignored
    Attempt attempt;              // the WiFi connection attempt, if any
    int64_t close_at_us = 0;      // 0 = no close scheduled
};
Session g_s;

NimBLEServer* g_server = nullptr;
NimBLECharacteristic* g_status = nullptr;
NimBLECharacteristic* g_error = nullptr;
NimBLECharacteristic* g_result = nullptr;
esp_timer_handle_t g_window_timer = nullptr;

int64_t now_us() { return esp_timer_get_time(); }

/// Sets the advert (state byte in the Improv service data) and makes sure it
/// is running unless a client is connected. NimBLE wants advertising stopped
/// while its data changes.
void advertise(State s) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv->isAdvertising() && !adv->stop()) ESP_LOGW(TAG, "could not stop advertising to update it");
    NimBLEAdvertisementData ad;
    ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    ad.addServiceUUID(NimBLEUUID(::improv::SERVICE_UUID));
    const uint8_t sd[6] = {static_cast<uint8_t>(s), kCapabilities, 0, 0, 0, 0};
    ad.setServiceData(NimBLEUUID(kServiceDataUuid), sd, sizeof(sd));
    NimBLEAdvertisementData scan;  // the 31-byte advert is full: the name goes here
    const std::string host = net::status().hostname;
    scan.setName(host.empty() ? std::string("DFU32") : host);
    if (!adv->setAdvertisementData(ad) || !adv->setScanResponseData(scan)) {
        ESP_LOGE(TAG, "setting the advert failed; scanners may see a stale state");
    }
    if (g_server->getConnectedCount() == 0 && !adv->start()) ESP_LOGE(TAG, "advertising failed to start");
}

void Host::publish_state(State s) {
    const auto v = static_cast<uint8_t>(s);
    g_status->setValue(&v, 1);
    g_status->notify();
    set_mirror(g_s.active, s);
    if (g_s.active && s != State::Stopped) advertise(s);
    ESP_LOGI(TAG, "state %u", v);
}

void Host::publish_error(Error e) {
    const auto v = static_cast<uint8_t>(e);
    g_error->setValue(&v, 1);
    g_error->notify();
    if (e != Error::None) ESP_LOGW(TAG, "error 0x%02x", v);
}

void Host::publish_result(std::span<const uint8_t> packet) {
    g_result->setValue(packet.data(), packet.size());
    g_result->notify();
}

bool Host::begin_connect(const std::string& ssid, const std::string& password) {
    return g_s.attempt.begin(ssid, password, kConnectWaitUs);
}

void Host::identify() {
    ESP_LOGI(TAG, "identify");
    g_identify_until_us = now_us() + kIdentifyUs;
}

DeviceInfo Host::device_info() {
    return {"DFU32", esp_app_get_description()->version, chip_name(CONFIG_IDF_TARGET), net::status().hostname,
            CONFIG_DFU32_BOARD_NAME};
}

/// Ends the connection attempt: keep the new network, or go back to the old one.
void finish_connect(bool ok) {
    if (ok) {
        g_core.connect_succeeded(g_s.attempt.keep());
        g_s.close_at_us = now_us() + kCloseAfterProvisionUs;
        return;
    }
    g_s.attempt.give_up();
    g_core.connect_failed();
}

void arm_window() {
    g_armed_gen = ++g_s.window_gen;
    esp_timer_stop(g_window_timer);  // ESP_ERR_INVALID_STATE when not running: fine
    if (esp_timer_start_once(g_window_timer, kButtonWindowUs) != ESP_OK) {
        ESP_LOGE(TAG, "could not arm the 5-minute window; `improv stop` closes it");
    }
}

bool do_open(Window why) {
    if (g_s.active) {
        if (why == Window::Button) arm_window();  // another press: 5 more minutes
        return true;
    }
    if (!job::try_acquire("improv")) {
        const char* h = job::holder();
        ESP_LOGW(TAG, "not opening Improv BLE: %s is using BLE", h ? h : "something");
        return false;
    }
    g_s.active = true;
    g_s.close_at_us = 0;
    ++g_s.window_gen;
    g_rpc_buf.reset();
    g_core.start();  // publishes Authorized, which starts advertising
    if (why == Window::Button) arm_window();
    ESP_LOGI(TAG, "Improv BLE open%s: set WiFi from Home Assistant or https://www.improv-wifi.com/",
             why == Window::Button ? " for 5 minutes" : "");
    return true;
}

void do_close() {
    if (!g_s.active) return;
    if (g_s.attempt.active()) finish_connect(false);  // closed mid-attempt: put the old network back
    ++g_s.window_gen;                           // any queued timer message is now stale
    esp_timer_stop(g_window_timer);
    g_s.close_at_us = 0;
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv->isAdvertising()) adv->stop();
    for (const uint16_t h : g_server->getPeerDevices()) g_server->disconnect(h);
    g_s.active = false;  // before stop(): publish_state must not restart advertising
    g_core.stop();
    g_rpc_buf.reset();
    set_mirror(false, State::Stopped);
    release_ble_lease();  // held since do_open(), released last (or kept by a Serial attempt)
    ESP_LOGI(TAG, "Improv BLE closed");
}

void handle(Msg& m) {
    switch (m.kind) {
        case MsgKind::Open: {
            const bool ok = do_open(m.window);
            if (m.ok != nullptr) *m.ok = ok;
            if (m.reply != nullptr) xSemaphoreGive(m.reply);
            break;
        }
        case MsgKind::Close: do_close(); break;
        case MsgKind::WindowExpired:
            if (g_s.active && m.gen == g_s.window_gen) {
                ESP_LOGI(TAG, "5-minute window over");
                do_close();
            }
            break;
        case MsgKind::Write:
            if (!g_s.active) break;
            switch (g_rpc_buf.push(m.data)) {
                case RpcAssembler::Status::Complete: g_core.handle_rpc(g_rpc_buf.packet()); break;
                case RpcAssembler::Status::TooLong: g_core.handle_rpc({}); break;  // reports INVALID_RPC
                case RpcAssembler::Status::Incomplete: break;
            }
            break;
        case MsgKind::ClientGone:
            g_rpc_buf.reset();  // a half-sent packet must not prefix the next client's
            if (g_s.active) advertise(g_core.state());
            break;
    }
}

void poll() {
    switch (g_s.attempt.poll()) {
        case Attempt::Poll::Joined: finish_connect(true); break;
        case Attempt::Poll::TimedOut: finish_connect(false); break;
        case Attempt::Poll::Idle:
        case Attempt::Poll::Pending: break;
    }
    if (g_s.close_at_us != 0 && now_us() >= g_s.close_at_us) do_close();
}

void improv_task(void*) {
    for (;;) {
        const bool busy = g_s.attempt.active() || g_s.close_at_us != 0;
        Msg* raw = nullptr;
        if (xQueueReceive(g_queue, &raw, busy ? kPollTicks : portMAX_DELAY) == pdTRUE && raw != nullptr) {
            std::unique_ptr<Msg> m(raw);
            handle(*m);
        }
        poll();
    }
}

// ---- NimBLE callbacks (host task): only post ------------------------------------------

class ServerCallbacks final : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
        ESP_LOGI(TAG, "client %s connected", info.getAddress().toString().c_str());
        auto m = std::make_unique<Msg>();
        m->kind = MsgKind::ClientGone;  // also clears the buffer for the new client
        post(std::move(m));
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        ESP_LOGI(TAG, "client disconnected (0x%02x)", reason);
        auto m = std::make_unique<Msg>();
        m->kind = MsgKind::ClientGone;
        post(std::move(m));
    }
};

class RpcCallbacks final : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        const NimBLEAttValue v = c->getValue();
        auto m = std::make_unique<Msg>();
        m->kind = MsgKind::Write;
        m->data.assign(v.data(), v.data() + v.size());
        post(std::move(m));
    }
};

ServerCallbacks g_server_cb;
RpcCallbacks g_rpc_cb;

void window_cb(void*) {
    auto m = std::make_unique<Msg>();
    m->kind = MsgKind::WindowExpired;
    m->gen = g_armed_gen.load();
    post(std::move(m));
}

}  // namespace

bool init() {
    if (!kEnabled || g_server != nullptr) return true;
    esp_timer_create_args_t w{};
    w.callback = &window_cb;
    w.name = "improv_window";
    if (esp_timer_create(&w, &g_window_timer) != ESP_OK) return false;
    g_queue = xQueueCreate(8, sizeof(Msg*));
    if (g_queue == nullptr || xTaskCreate(&improv_task, "improv", 5120, nullptr, 4, nullptr) != pdPASS) return false;

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(&g_server_cb, false);
    g_server->advertiseOnDisconnect(false);
    NimBLEService* svc = g_server->createService(::improv::SERVICE_UUID);
    g_status = svc->createCharacteristic(::improv::STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_error = svc->createCharacteristic(::improv::ERROR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* rpc = svc->createCharacteristic(::improv::RPC_COMMAND_UUID, NIMBLE_PROPERTY::WRITE);
    rpc->setCallbacks(&g_rpc_cb);
    g_result = svc->createCharacteristic(::improv::RPC_RESULT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* caps = svc->createCharacteristic(::improv::CAPABILITIES_UUID, NIMBLE_PROPERTY::READ);
    caps->setValue(&kCapabilities, 1);
    const uint8_t zero = 0;
    g_status->setValue(&zero, 1);
    g_error->setValue(&zero, 1);
    return true;
}

bool open(Window why) {
    if (!kEnabled || g_queue == nullptr) return false;
    SemaphoreHandle_t reply = xSemaphoreCreateBinary();
    if (reply == nullptr) return false;
    // `ok` and `reply` outlive the wait below, or are leaked on purpose if the
    // task never answers (it then still writes to them).
    auto* ok = new bool(false);
    auto m = std::make_unique<Msg>();
    m->kind = MsgKind::Open;
    m->window = why;
    m->reply = reply;
    m->ok = ok;
    if (!post(std::move(m))) {
        vSemaphoreDelete(reply);
        delete ok;
        return false;
    }
    if (xSemaphoreTake(reply, kOpenReplyTicks) != pdTRUE) {
        ESP_LOGE(TAG, "Improv task did not answer");
        return false;
    }
    const bool result = *ok;
    vSemaphoreDelete(reply);
    delete ok;
    return result;
}

bool close() {
    const bool was = snapshot().active;
    if (!was) return false;
    auto m = std::make_unique<Msg>();
    m->kind = MsgKind::Close;
    post(std::move(m));
    return true;
}

Snapshot snapshot() {
    std::lock_guard<std::mutex> lk(g_mirror_mu);
    return g_mirror;
}

bool active() { return snapshot().active; }

bool identifying() { return esp_timer_get_time() < g_identify_until_us.load(); }

}  // namespace dfu32::improv
