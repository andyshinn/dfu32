// SPDX-License-Identifier: MIT
#include "dfu_ble/dfu_ble.hpp"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>

#include "NimBLEDevice.h"
#include "dfu_proto/chunk.hpp"
#include "dfu_proto/protocol.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "sdkconfig.h"

namespace dfu32::ble {
namespace {

constexpr const char* TAG = "dfu_ble";
constexpr uint16_t kPreferredMtu = 247;
constexpr uint32_t kBootloaderScanMs = 30000;
constexpr uint32_t kJumpDisconnectMs = 5000;
constexpr uint32_t kMtuWaitMs = 1500;
constexpr uint16_t kDefaultConnInterval = 16;  // x1.25 ms = 20 ms
constexpr uint32_t kPacketRetryLimit = 200;  // x 1 tick of back-off on ENOMEM

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void sleep_ms(uint32_t ms) {
    if (ms == 0) return;
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t == 0 ? 1 : t);
}

NimBLEAddress to_nimble(const Mac& m) {
    ble_addr_t a{};
    a.type = m.type;
    std::memcpy(a.val, m.val.data(), 6);
    return NimBLEAddress(a);
}

Mac from_nimble(const NimBLEAddress& a) {
    Mac m;
    std::memcpy(m.val.data(), a.getVal(), 6);
    m.type = a.getType();
    return m;
}

const NimBLEUUID& dfu_uuid() {
    static const NimBLEUUID u(dfu::kServiceUuid);
    return u;
}

// ---- scanning -----------------------------------------------------------------

const NimBLEUUID& secure_dfu_uuid() {
    static const NimBLEUUID u("FE59");
    return u;
}

class ScanCallbacks : public NimBLEScanCallbacks {
public:
    // Target search by address only: the app MAC, or MAC+1 (bootloader after a
    // 0xA8 reset). Advertising content is not required to match: the MeshCore
    // app's OTA adverts carry no DFU UUID, and some bootloader builds put it in
    // the scan response or omit it. The GATT Version characteristic, read after
    // connecting, is what proves app (0x0001) vs bootloader (0x0008).
    void arm(const Mac& app, SemaphoreHandle_t done) {
        app_ = app;
        plus_one_ = app.plus_one();
        done_ = done;
        found_ = false;
        notable_count_ = 0;
        searching_ = true;
    }
    void disarm() {
        searching_ = false;
        done_ = nullptr;
    }
    bool found() const { return found_; }
    const Advert& hit() const { return hit_; }
    bool hit_plus_one() const { return hit_plus_one_; }

    /// DFU-looking advertisers seen during a search that did not match the target.
    size_t notable_count() const { return std::min<size_t>(notable_count_.load(), notable_.size()); }
    const Advert& notable(size_t i) const { return notable_[i]; }
    bool notable_secure(size_t i) const { return notable_secure_[i]; }

    void onDiscovered(const NimBLEAdvertisedDevice* dev) override { consider(dev); }
    void onResult(const NimBLEAdvertisedDevice* dev) override { consider(dev); }  // after scan response

    void onScanEnd(const NimBLEScanResults&, int) override {
        if (!searching_) return;
        if (SemaphoreHandle_t d = done_.load()) xSemaphoreGive(d);
    }

private:
    void consider(const NimBLEAdvertisedDevice* dev) {
        if (!searching_ || found_) return;
        const uint8_t* v = dev->getAddress().getVal();
        const bool exact = app_.same_bytes(v);
        const bool plus = plus_one_.same_bytes(v);
        const bool has_uuid = dev->isAdvertisingService(dfu_uuid());
        if (!exact && !plus) {
            note(dev, has_uuid);
            return;
        }
        hit_.mac = from_nimble(dev->getAddress());
        hit_.name = dev->haveName() ? dev->getName() : "";
        hit_.rssi = dev->getRSSI();
        hit_.dfu_service = has_uuid;
        hit_.connectable = dev->isConnectable();
        hit_plus_one_ = plus && !exact;
        found_ = true;
        NimBLEDevice::getScan()->stop();
        if (SemaphoreHandle_t d = done_.load()) xSemaphoreGive(d);
    }

    void note(const NimBLEAdvertisedDevice* dev, bool has_uuid) {
        const bool secure = dev->isAdvertisingService(secure_dfu_uuid());
        const std::string name = dev->haveName() ? dev->getName() : "";
        const bool dfu_name = name.find("DFU") != std::string::npos || name.find("dfu") != std::string::npos;
        if (!has_uuid && !secure && !dfu_name) return;
        const Mac m = from_nimble(dev->getAddress());
        const size_t n = notable_count();
        for (size_t i = 0; i < n; ++i) {
            if (notable_[i].mac.val == m.val) {
                if (!name.empty()) notable_[i].name = name;  // scan response may add the name
                return;
            }
        }
        if (n >= notable_.size()) return;
        notable_[n].mac = m;
        notable_[n].name = name;
        notable_[n].rssi = dev->getRSSI();
        notable_[n].dfu_service = has_uuid;
        notable_secure_[n] = secure;
        notable_count_.store(n + 1);
    }

    Mac app_, plus_one_;
    std::atomic<bool> searching_{false};
    std::atomic<bool> found_{false};
    bool hit_plus_one_ = false;
    Advert hit_;
    std::atomic<SemaphoreHandle_t> done_{nullptr};
    std::array<Advert, 8> notable_{};
    std::array<bool, 8> notable_secure_{};
    std::atomic<size_t> notable_count_{0};
};

ScanCallbacks g_scan_cb;

bool find_target(const Mac& app, uint32_t timeout_ms, const char* what, Advert& out, bool& plus_one) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    g_scan_cb.arm(app, done);
    scan->clearResults();
    ESP_LOGI(TAG, "scanning up to %" PRIu32 " s for the %s at %s or MAC+1 %s", timeout_ms / 1000, what,
             app.str().c_str(), app.plus_one().str().c_str());
    bool ok = false;
    if (scan->start(timeout_ms, false, true)) {
        xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms + 1000));
        ok = g_scan_cb.found();
        scan->stop();
    } else {
        ESP_LOGE(TAG, "scan start failed");
    }
    g_scan_cb.disarm();
    vSemaphoreDelete(done);
    if (ok) {
        out = g_scan_cb.hit();
        plus_one = g_scan_cb.hit_plus_one();
        ESP_LOGI(TAG, "found %s%s \"%s\" rssi %d dfu_uuid=%d", out.mac.str().c_str(), plus_one ? " (MAC+1)" : "",
                 out.name.c_str(), out.rssi, out.dfu_service);
        return true;
    }
    const size_t n = g_scan_cb.notable_count();
    if (n == 0) {
        ESP_LOGW(TAG, "no %s at either address, and no other DFU-looking advertisers seen", what);
    }
    for (size_t i = 0; i < n; ++i) {
        const Advert& a = g_scan_cb.notable(i);
        ESP_LOGW(TAG, "not the target, but DFU-looking: %s \"%s\" rssi %d%s%s", a.mac.str().c_str(), a.name.c_str(),
                 a.rssi, a.dfu_service ? " [legacy DFU 1530]" : "",
                 g_scan_cb.notable_secure(i) ? " [Secure DFU FE59: not supported]" : "");
    }
    return false;
}

// ---- connection / transport -----------------------------------------------------

class Link final : public dfu::ITransport, public NimBLEClientCallbacks {
public:
    Link() : queue_(xQueueCreate(16, sizeof(dfu::RawNotification))) {}
    ~Link() override {
        close();
        vQueueDelete(queue_);
    }
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    bool connect(const Mac& mac, const ConnParams& conn, std::string& err) {
        client_ = NimBLEDevice::createClient();
        if (client_ == nullptr) {
            err = "createClient failed (too many clients?)";
            return false;
        }
        client_->setClientCallbacks(this, false);
        // Default 20 ms (bench: 30 ms 9.2 kB/s, 20 ms 12.4, 15 ms 14.8; 11.25 ms and
        // faster overran OTAFIX's lazy-erase buffering). See docs/BENCH.md.
        uint16_t itvl_min = kDefaultConnInterval, itvl_max = kDefaultConnInterval;
        if (conn.interval_ms != 0) {
            itvl_min = itvl_max = std::max<uint16_t>(6, static_cast<uint16_t>(conn.interval_ms * 4 / 5));
        }
        client_->setConnectionParams(itvl_min, itvl_max, 0, 200);  // latency 0, 2 s supervision
        client_->setConnectTimeout(10000);
        mtu_changed_ = false;
        if (!client_->connect(to_nimble(mac), true, false, true)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "connect to %s failed (rc %d)", mac.str().c_str(), client_->getLastError());
            err = buf;
            return false;
        }
#if CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT
        client_->updatePhy(BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_CODED_ANY);
#endif
        client_->setDataLen(251);
        return true;
    }

    bool discover(std::string& err) {
        NimBLERemoteService* svc = client_->getService(dfu::kServiceUuid);
        if (svc == nullptr) {
            err = "DFU service 0x1530 not found (is the target in OTA mode?)";
            return false;
        }
        cp_ = svc->getCharacteristic(dfu::kControlPointUuid);
        pkt_ = svc->getCharacteristic(dfu::kPacketUuid);
        ver_ = svc->getCharacteristic(dfu::kVersionUuid);
        if (cp_ == nullptr || pkt_ == nullptr) {
            err = "DFU Control Point / Packet characteristic missing";
            return false;
        }
        // MTU exchange is asynchronous; give it a moment to land.
        const uint32_t until = now_ms() + kMtuWaitMs;
        while (!mtu_changed_ && client_->getMTU() <= dfu::kAttMtuMin && now_ms() < until) {
            sleep_ms(20);
        }
        return true;
    }

    uint16_t read_version() {
        if (ver_ == nullptr) return 0;
        const NimBLEAttValue v = ver_->readValue();
        return v.size() >= 2 ? static_cast<uint16_t>(v.data()[0] | (v.data()[1] << 8)) : 0;
    }

    std::string read_dis(const char* chr) {
        NimBLERemoteService* dis = client_->getService("180A");
        if (dis == nullptr) return {};
        NimBLERemoteCharacteristic* c = dis->getCharacteristic(chr);
        if (c == nullptr || !c->canRead()) return {};
        const NimBLEAttValue v = c->readValue();
        std::string s(v.c_str(), v.size());
        while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
        return s;
    }

    bool enable_notifications(std::string& err) {
        const bool ok = cp_->subscribe(
            true,
            [this](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
                dfu::RawNotification n;
                n.len = static_cast<uint8_t>(std::min(len, n.data.size()));
                std::memcpy(n.data.data(), data, n.len);
                if (xQueueSend(queue_, &n, 0) != pdTRUE) {
                    dropped_.fetch_add(1);
                }
            },
            true);
        if (!ok) err = "enabling Control Point notifications failed";
        return ok;
    }

    void read_phy(uint8_t& tx, uint8_t& rx) {
        tx = rx = 0;
#if CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT
        client_->getPhy(&tx, &rx);
#else
        tx = rx = 1;  // BLE 4.2 controller: 1M only
#endif
    }

    uint16_t mtu() const { return client_ ? client_->getMTU() : 0; }
    uint32_t enomem_retries() const { return enomem_retries_; }
    uint32_t dropped_notifications() const { return dropped_.load(); }

    bool wait_disconnect(uint32_t ms) {
        const uint32_t until = now_ms() + ms;
        while (is_connected() && now_ms() < until) sleep_ms(20);
        return !is_connected();
    }

    void close() {
        if (client_ == nullptr) return;
        if (client_->isConnected()) {
            client_->disconnect();
            wait_disconnect(2000);
        }
        NimBLEDevice::deleteClient(client_);
        client_ = nullptr;
        cp_ = pkt_ = ver_ = nullptr;
    }

    // ---- dfu::ITransport ----
    bool write_control(std::span<const uint8_t> d) override {
        return is_connected() && cp_->writeValue(d.data(), d.size(), true);
    }

    bool write_packet(std::span<const uint8_t> d) override {
        // Raw NimBLE call: esp-nimble-cpp's writeValue() turns oversize writes
        // into long writes with response and logs every ENOMEM.
        if (d.size() + dfu::kAttHeaderSize > mtu()) {
            ESP_LOGE(TAG, "packet %u B exceeds MTU %u", static_cast<unsigned>(d.size()), mtu());
            return false;
        }
        for (uint32_t attempt = 0; attempt < kPacketRetryLimit; ++attempt) {
            if (!is_connected()) return false;
            const int rc = ble_gattc_write_no_rsp_flat(client_->getConnHandle(), pkt_->getHandle(), d.data(),
                                                       static_cast<uint16_t>(d.size()));
            if (rc == 0) return true;
            if (rc != BLE_HS_ENOMEM) {
                ESP_LOGE(TAG, "write_no_rsp rc=%d", rc);
                return false;
            }
            ++enomem_retries_;
            vTaskDelay(1);
        }
        ESP_LOGE(TAG, "BLE TX stalled (out of mbufs for %" PRIu32 " ticks)", kPacketRetryLimit);
        return false;
    }

    bool wait_notification(dfu::RawNotification& out, uint32_t timeout_ms) override {
        TickType_t t = pdMS_TO_TICKS(timeout_ms);
        if (t == 0 && timeout_ms > 0) t = 1;
        return xQueueReceive(queue_, &out, t) == pdTRUE;
    }

    void flush_notifications() override { xQueueReset(queue_); }
    bool is_connected() const override { return client_ != nullptr && client_->isConnected(); }
    void delay_ms(uint32_t ms) override { sleep_ms(ms); }
    uint32_t now_ms() override { return dfu32::ble::now_ms(); }

    // ---- NimBLEClientCallbacks ----
    void onDisconnect(NimBLEClient*, int reason) override {
        ESP_LOGI(TAG, "disconnected (reason 0x%02x)", reason);
    }
    void onMTUChange(NimBLEClient*, uint16_t mtu) override {
        ESP_LOGI(TAG, "MTU %u", mtu);
        mtu_changed_ = true;
    }
    bool onConnParamsUpdateRequest(NimBLEClient*, const ble_gap_upd_params* p) override {
        ESP_LOGI(TAG, "peer requests conn params: interval %u-%u (x1.25 ms), latency %u, timeout %u (x10 ms); accepting",
                 p->itvl_min, p->itvl_max, p->latency, p->supervision_timeout);
        return true;
    }

    void read_conn(uint16_t& interval, uint16_t& latency, uint16_t& timeout) const {
        interval = latency = timeout = 0;
        if (!is_connected()) return;
        const NimBLEConnInfo ci = client_->getConnInfo();
        interval = ci.getConnInterval();
        latency = ci.getConnLatency();
        timeout = ci.getConnTimeout();
    }

    void onPhyUpdate(NimBLEClient*, uint8_t tx, uint8_t rx) override {
        ESP_LOGI(TAG, "PHY tx %u rx %u", tx, rx);
    }

private:
    QueueHandle_t queue_;
    NimBLEClient* client_ = nullptr;
    NimBLERemoteCharacteristic* cp_ = nullptr;
    NimBLERemoteCharacteristic* pkt_ = nullptr;
    NimBLERemoteCharacteristic* ver_ = nullptr;
    std::atomic<bool> mtu_changed_{false};
    std::atomic<uint32_t> dropped_{0};
    uint32_t enomem_retries_ = 0;
};

/// Finds the target and returns a connected Link to its bootloader, performing
/// the buttonless jump if the target is still in its app.
std::unique_ptr<Link> open_bootloader(const Mac& app, uint32_t scan_ms, const ConnParams& conn, LinkInfo& info,
                                      std::string& err) {
    Advert adv;
    bool plus_one = false;
    uint32_t t0 = now_ms();
    if (!find_target(app, scan_ms, "app or bootloader", adv, plus_one)) {
        err = "target not found: run `start ota` on the repeater (or check the MAC)";
        return nullptr;
    }
    info.find_ms = now_ms() - t0;

    auto link = std::make_unique<Link>();
    t0 = now_ms();
    if (!link->connect(adv.mac, conn, err) || !link->discover(err)) return nullptr;
    uint16_t version = link->read_version();
    ESP_LOGI(TAG, "connected to %s, DFU Version char 0x%04x, MTU %u", adv.mac.str().c_str(), version, link->mtu());

    if (version == dfu::kVersionAppButtonless) {
        // Bluefruit BLEDfu: CCCD on, START -> app saves peer data, sets
        // GPREGRET=0xB1 and jumps to the bootloader (same address).
        const uint32_t tj = now_ms();
        if (!link->enable_notifications(err)) return nullptr;
        const auto start = dfu::encode_start(dfu::ImageType::Application);
        (void)link->write_control(start);  // link usually drops before the ATT response
        if (!link->wait_disconnect(kJumpDisconnectMs)) {
            ESP_LOGW(TAG, "app did not drop the link after buttonless START; disconnecting");
        }
        link->close();
        info.via_buttonless = true;
        sleep_ms(300);
        if (!find_target(app, kBootloaderScanMs, "bootloader", adv, plus_one)) {
            err = "bootloader did not appear after the buttonless jump (see warnings above for what was seen)";
            return nullptr;
        }
        info.jump_ms = now_ms() - tj;
        link = std::make_unique<Link>();
        t0 = now_ms();
        if (!link->connect(adv.mac, conn, err) || !link->discover(err)) return nullptr;
        version = link->read_version();
        ESP_LOGI(TAG, "connected to bootloader %s, Version 0x%04x, MTU %u", adv.mac.str().c_str(), version,
                 link->mtu());
    }

    if (version != dfu::kVersionBootloader) {
        char buf[120];
        if (info.via_buttonless && version == dfu::kVersionAppButtonless) {
            std::snprintf(buf, sizeof(buf), "the app is still running at %s after the buttonless START (no jump)",
                          adv.mac.str().c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "unexpected DFU Version 0x%04x (want 0x0008 bootloader)", version);
        }
        err = buf;
        return nullptr;
    }

    info.connect_ms = now_ms() - t0;
    info.address = adv.mac;
    info.found_at_plus_one = plus_one;
    info.version_char = version;
    info.mtu = link->mtu();
    link->read_phy(info.tx_phy, info.rx_phy);
    link->read_conn(info.conn_interval, info.conn_latency, info.conn_timeout);
    info.dis_fw_rev = link->read_dis("2A26");
    info.dis_model = link->read_dis("2A24");
    info.bootloader = dfu::classify_bootloader(info.dis_fw_rev);
    info.packet_size = dfu::packet_size_for_mtu(info.mtu, dfu::max_packet_for(info.bootloader.cls));
    ESP_LOGI(TAG, "bootloader \"%s\" (%s, model \"%s\"), MTU %u -> packet %u B, PHY %u/%u",
             info.dis_fw_rev.c_str(), dfu::to_string(info.bootloader.cls), info.dis_model.c_str(), info.mtu,
             info.packet_size, info.tx_phy, info.rx_phy);
    if (info.bootloader.cls == dfu::BootloaderClass::UpstreamLe0_9) {
        ESP_LOGW(TAG, "upstream bootloader <= 0.9.x: a failed transfer needs USB to recover");
    } else if (info.bootloader.cls == dfu::BootloaderClass::Nrf54) {
        ESP_LOGI(TAG, "nRF54 bootloader: .dat device type must be 0x0054");
    } else if (info.bootloader.cls == dfu::BootloaderClass::Unknown) {
        ESP_LOGW(TAG, "bootloader not recognised; using 20-byte packets");
    }
    return link;
}

void send_sys_reset(Link& link) {
    std::string err;
    if (!link.is_connected()) return;
    if (!link.enable_notifications(err)) {
        ESP_LOGW(TAG, "%s", err.c_str());
    }
    (void)link.write_control(dfu::encode_op(dfu::Op::SystemReset));
    link.wait_disconnect(3000);
}

}  // namespace

// ---- Mac ------------------------------------------------------------------------

bool Mac::parse(const std::string& text, uint8_t type, Mac& out) {
    unsigned b[6];
    char tail;
    if (std::sscanf(text.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) out.val[5 - i] = static_cast<uint8_t>(b[i]);
    out.type = type;
    return true;
}

std::string Mac::str() const {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", val[5], val[4], val[3], val[2], val[1], val[0]);
    return buf;
}

Mac Mac::plus_one() const {
    Mac m = *this;
    m.val[0] = static_cast<uint8_t>(m.val[0] + 1);
    return m;
}

bool Mac::same_bytes(const uint8_t* lsb_first) const { return std::memcmp(val.data(), lsb_first, 6) == 0; }

// ---- public API ---------------------------------------------------------------

bool init() {
    if (!NimBLEDevice::init("DFU32")) {
        ESP_LOGE(TAG, "NimBLE init failed");
        return false;
    }
    NimBLEDevice::setMTU(kPreferredMtu);
    NimBLEDevice::setPower(9);
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&g_scan_cb, false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(100);
    ESP_LOGI(TAG, "NimBLE up, own address %s", NimBLEDevice::getAddress().toString().c_str());
    return true;
}

std::vector<Advert> scan(uint32_t duration_ms) {
    std::vector<Advert> out;
    NimBLEScan* s = NimBLEDevice::getScan();
    s->clearResults();
    const NimBLEScanResults results = s->getResults(duration_ms, false);
    for (const NimBLEAdvertisedDevice* dev : results) {
        Advert a;
        a.mac = from_nimble(dev->getAddress());
        a.name = dev->haveName() ? dev->getName() : "";
        a.rssi = dev->getRSSI();
        a.dfu_service = dev->isAdvertisingService(dfu_uuid());
        a.connectable = dev->isConnectable();
        out.push_back(std::move(a));
    }
    s->clearResults();
    std::sort(out.begin(), out.end(), [](const Advert& x, const Advert& y) { return x.rssi > y.rssi; });
    return out;
}

ProbeReport probe(const Mac& app, uint32_t scan_ms, bool reset_after, ConnParams conn) {
    ProbeReport r;
    auto link = open_bootloader(app, scan_ms, conn, r.link, r.error);
    if (!link) return r;
    r.ok = true;
    if (reset_after) {
        send_sys_reset(*link);
        r.reset_sent = true;
    }
    link->close();
    return r;
}

FlashReport flash(const Mac& app, dfu::IImageSource& image, uint32_t image_size,
                  std::span<const uint8_t> init_packet, const dfu::InitPacket& init, const FlashOptions& opts) {
    FlashReport r;
    const uint32_t t0 = now_ms();
    auto link = open_bootloader(app, opts.scan_ms, opts.conn, r.link, r.error);
    if (!link) {
        r.total_ms = now_ms() - t0;
        return r;
    }

    const uint16_t want_type = r.link.bootloader.device_type;
    if (want_type != 0 && init.device_type != want_type) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "DEVICE_TYPE_MISMATCH: image is for device type 0x%04X, bootloader wants 0x%04X",
                      init.device_type, want_type);
        r.error = buf;
        send_sys_reset(*link);  // nothing erased; let the app boot again
        link->close();
        r.total_ms = now_ms() - t0;
        return r;
    }

    const uint16_t fwid = r.link.bootloader.sd_fwid;
    if (fwid != 0 && !init.accepts_softdevice(fwid)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "SD_REQ_MISMATCH: target SoftDevice 0x%04X not in .dat sd_req", fwid);
        r.error = buf;
        send_sys_reset(*link);  // nothing erased; let the app boot again
        link->close();
        r.total_ms = now_ms() - t0;
        return r;
    }

    std::string err;
    if (!link->enable_notifications(err)) {
        r.error = err;
        send_sys_reset(*link);
        link->close();
        r.total_ms = now_ms() - t0;
        return r;
    }

    dfu::TransferParams params;
    params.packet_size = r.link.packet_size;
    if (opts.max_packet != 0) {
        params.packet_size = std::min<uint16_t>(params.packet_size, static_cast<uint16_t>(opts.max_packet & ~3u));
    }
    params.prn = opts.prn;
    params.start_settle_ms = opts.start_settle_ms;

    link->read_conn(r.link.conn_interval, r.link.conn_latency, r.link.conn_timeout);
    ESP_LOGI(TAG, "transfer: %" PRIu32 " B in %u-byte packets, PRN %u, conn interval %u x1.25 ms, latency %u",
             image_size, params.packet_size, params.prn, r.link.conn_interval, r.link.conn_latency);
    uint32_t next_pct = 0;
    r.transfer = dfu::run_transfer(*link, image, image_size, init_packet, params,
                                   [&](uint32_t acked, uint32_t total) {
                                       const uint32_t pct = static_cast<uint32_t>(uint64_t{acked} * 100 / total);
                                       if (pct >= next_pct) {
                                           ESP_LOGI(TAG, "progress %3" PRIu32 "%% (%" PRIu32 "/%" PRIu32 ")", pct,
                                                    acked, total);
                                           next_pct = pct + 10;
                                       }
                                   });
    if (link->enomem_retries() || link->dropped_notifications()) {
        ESP_LOGI(TAG, "tx back-off retries %" PRIu32 ", dropped notifications %" PRIu32, link->enomem_retries(),
                 link->dropped_notifications());
    }

    if (r.transfer.ok()) {
        r.ok = true;
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s in %s (status 0x%02x)", dfu::to_string(r.transfer.error),
                      dfu::to_string(r.transfer.phase), r.transfer.status);
        r.error = buf;
        if (link->is_connected()) {
            // Clear the half-finished session so the next attempt can START
            // (OTAFIX / upstream >= 0.10 come back in BLE OTA, possibly at MAC+1).
            send_sys_reset(*link);
            r.sys_reset_after_failure = true;
        }
    }
    link->close();
    r.total_ms = now_ms() - t0;
    return r;
}

bool reset_bootloader(const Mac& app, uint32_t scan_ms, std::string& error) {
    Advert adv;
    bool plus_one = false;
    if (!find_target(app, scan_ms, "bootloader", adv, plus_one)) {
        error = "nothing advertising at the target MAC or MAC+1";
        return false;
    }
    Link link;
    if (!link.connect(adv.mac, ConnParams{}, error) || !link.discover(error)) return false;
    send_sys_reset(link);
    link.close();
    return true;
}

}  // namespace dfu32::ble
