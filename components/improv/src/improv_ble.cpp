// SPDX-License-Identifier: MIT
#include "improv/improv.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "NimBLEDevice.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "improv.h"  // vendored improv-wifi/sdk-cpp: UUIDs
#include "job/job.hpp"
#include "net/net.hpp"
#include "sdkconfig.h"

namespace dfu32::improv {
namespace {

constexpr const char* TAG = "improv";
constexpr uint32_t kConnectWaitMs = 30000;                // per attempt
constexpr uint64_t kButtonWindowUs = 5ULL * 60 * 1000000;  // SPEC.md §9.2
constexpr uint64_t kCloseAfterProvisionUs = 5ULL * 1000000;
constexpr int64_t kIdentifyUs = 10LL * 1000000;
constexpr uint16_t kServiceDataUuid = 0x4677;              // Improv BLE advertisement service data
constexpr uint8_t kCapabilities = ::improv::CAPABILITY_IDENTIFY;
#ifdef CONFIG_DFU32_IMPROV_BLE
constexpr bool kEnabled = true;
#else
constexpr bool kEnabled = false;
#endif

struct Msg {
    enum class Kind : uint8_t { Connect, Close } kind = Kind::Close;
    std::string ssid;
    std::string password;
};

class Host final : public IHost {
public:
    void publish_state(State s) override;
    void publish_error(Error e) override;
    void publish_result(std::span<const uint8_t> packet) override;
    void begin_connect(const std::string& ssid, const std::string& password) override;
    void identify() override;
    DeviceInfo device_info() override;
};

Host g_host;
Core g_core(g_host);    // guarded by g_mu
RpcAssembler g_rpc_buf; // guarded by g_mu
std::mutex g_mu;

NimBLEServer* g_server = nullptr;
NimBLECharacteristic* g_status = nullptr;
NimBLECharacteristic* g_error = nullptr;
NimBLECharacteristic* g_result = nullptr;
QueueHandle_t g_queue = nullptr;
esp_timer_handle_t g_window_timer = nullptr;
esp_timer_handle_t g_close_timer = nullptr;
std::atomic<bool> g_active{false};
std::atomic<int64_t> g_identify_until_us{0};

void post(Msg::Kind kind, const std::string& ssid = {}, const std::string& password = {}) {
    auto* m = new Msg{kind, ssid, password};
    if (g_queue == nullptr || xQueueSend(g_queue, &m, 0) != pdTRUE) {
        ESP_LOGE(TAG, "queue full; dropping a request");
        delete m;
    }
}

void advertise(State s) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData ad;
    ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    ad.addServiceUUID(NimBLEUUID(::improv::SERVICE_UUID));
    const uint8_t sd[6] = {static_cast<uint8_t>(s), kCapabilities, 0, 0, 0, 0};
    ad.setServiceData(NimBLEUUID(kServiceDataUuid), sd, sizeof(sd));
    NimBLEAdvertisementData scan;  // the 31-byte advert is full: the name goes here
    scan.setName(net::status().hostname.empty() ? std::string("DFU32") : net::status().hostname);
    adv->setAdvertisementData(ad);
    adv->setScanResponseData(scan);
    if (!adv->isAdvertising() && g_server->getConnectedCount() == 0 && !adv->start()) {
        ESP_LOGE(TAG, "advertising failed to start");
    }
}

void Host::publish_state(State s) {
    const auto v = static_cast<uint8_t>(s);
    g_status->setValue(&v, 1);
    g_status->notify();
    if (g_active && s != State::Stopped) advertise(s);  // keep the advert's state byte current
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

void Host::begin_connect(const std::string& ssid, const std::string& password) {
    ESP_LOGI(TAG, "WiFi settings received for \"%s\"; connecting", ssid.c_str());
    post(Msg::Kind::Connect, ssid, password);
}

void Host::identify() {
    ESP_LOGI(TAG, "identify");
    g_identify_until_us = esp_timer_get_time() + kIdentifyUs;
}

DeviceInfo Host::device_info() {
    return {"DFU32", esp_app_get_description()->version, chip_name(CONFIG_IDF_TARGET), net::status().hostname};
}

class ServerCallbacks final : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
        ESP_LOGI(TAG, "client %s connected", info.getAddress().toString().c_str());
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        ESP_LOGI(TAG, "client disconnected (0x%02x)", reason);
        if (g_active) NimBLEDevice::getAdvertising()->start();
    }
};

class RpcCallbacks final : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        const NimBLEAttValue v = c->getValue();
        std::lock_guard<std::mutex> lk(g_mu);
        switch (g_rpc_buf.push({v.data(), v.size()})) {
            case RpcAssembler::Status::Complete: g_core.handle_rpc(g_rpc_buf.packet()); break;
            case RpcAssembler::Status::TooLong: g_core.handle_rpc({}); break;  // reports INVALID_RPC
            case RpcAssembler::Status::Incomplete: break;
        }
    }
};

ServerCallbacks g_server_cb;
RpcCallbacks g_rpc_cb;

void do_connect(const Msg& m) {
    std::string old_ssid, old_pass;
    const bool had = net::stored_credentials(old_ssid, old_pass);
    bool ok = net::set_credentials(m.ssid, m.password) && net::wait_connected(kConnectWaitMs);
    if (ok) {
        const std::string url = "http://" + net::status().ip + "/";
        ESP_LOGI(TAG, "connected to \"%s\"; provisioned, redirect %s", m.ssid.c_str(), url.c_str());
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_core.connect_succeeded(url);
        }
        esp_timer_start_once(g_close_timer, kCloseAfterProvisionUs);  // let the client read the result
        return;
    }
    ESP_LOGW(TAG, "could not join \"%s\" within %u s; %s", m.ssid.c_str(), static_cast<unsigned>(kConnectWaitMs / 1000),
             had ? "going back to the previous network" : "clearing it again");
    if (had) {
        net::set_credentials(old_ssid, old_pass);
    } else {
        net::forget();
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_core.connect_failed();
}

void do_close() {
    if (!g_active.exchange(false)) return;
    esp_timer_stop(g_window_timer);
    esp_timer_stop(g_close_timer);
    NimBLEDevice::getAdvertising()->stop();
    for (const uint16_t h : g_server->getPeerDevices()) g_server->disconnect(h);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_core.stop();
        g_rpc_buf.reset();
    }
    job::release();  // held since open()
    ESP_LOGI(TAG, "Improv BLE closed");
}

void improv_task(void*) {
    for (;;) {
        Msg* raw = nullptr;
        if (xQueueReceive(g_queue, &raw, portMAX_DELAY) != pdTRUE || raw == nullptr) continue;
        std::unique_ptr<Msg> m(raw);
        if (m->kind == Msg::Kind::Connect) {
            do_connect(*m);
        } else {
            do_close();
        }
    }
}

void close_cb(void*) { post(Msg::Kind::Close); }

}  // namespace

bool init() {
    if (!kEnabled) return true;
    if (g_server != nullptr) return true;
    esp_timer_create_args_t w{};
    w.callback = &close_cb;
    w.name = "improv_window";
    esp_timer_create_args_t c{};
    c.callback = &close_cb;
    c.name = "improv_close";
    if (esp_timer_create(&w, &g_window_timer) != ESP_OK || esp_timer_create(&c, &g_close_timer) != ESP_OK) return false;
    g_queue = xQueueCreate(4, sizeof(Msg*));
    if (g_queue == nullptr || xTaskCreate(&improv_task, "improv", 4096, nullptr, 4, nullptr) != pdPASS) return false;

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
    if (!kEnabled || g_server == nullptr) return false;
    if (g_active) {
        if (why == Window::Button) esp_timer_restart(g_window_timer, kButtonWindowUs);
        return true;
    }
    if (!job::try_acquire("improv")) {
        const char* h = job::holder();
        ESP_LOGW(TAG, "not starting Improv BLE: %s is using BLE", h ? h : "something");
        return false;
    }
    g_active = true;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_rpc_buf.reset();
        g_core.start();  // publishes Authorized and starts advertising
    }
    if (why == Window::Button) esp_timer_start_once(g_window_timer, kButtonWindowUs);
    ESP_LOGI(TAG, "Improv BLE open%s: set WiFi from Home Assistant or https://www.improv-wifi.com/",
             why == Window::Button ? " for 5 minutes" : "");
    return true;
}

void close() {
    if (g_active) post(Msg::Kind::Close);
}

bool active() { return g_active; }

State state() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_core.state();
}

bool identifying() { return esp_timer_get_time() < g_identify_until_us.load(); }

}  // namespace dfu32::improv
