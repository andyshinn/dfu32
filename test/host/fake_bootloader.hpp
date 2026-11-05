// SPDX-License-Identifier: MIT
//
// Simulated Adafruit-bootloader legacy DFU server implementing dfu::ITransport,
// modelled on the behaviour verified from bootloader source (docs/PROTOCOL_NOTES.md):
//  - Packet writes that arrive before START is processed are dropped silently
//    (modelled: size packet written without an intervening delay is dropped)
//  - START needs exactly 12 bytes, sizes multiple of 4, app only, IDLE state
//  - INIT response only after 02 01; device_type must be 0x0052
//  - data packets must be multiples of 4 (else 10 03 03)
//  - PRN every N packets with the cumulative byte count
//  - VALIDATE compares CRC16 of received data with the init packet CRC
//  - ACTIVATE drops the link without a response
// Uses a virtual clock so timeouts run instantly.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "dfu_proto/crc16.hpp"
#include "dfu_proto/init_packet.hpp"
#include "dfu_proto/transfer.hpp"

class FakeBootloader : public dfu::ITransport {
public:
    // ---- fault injection ----
    std::optional<uint32_t> disconnect_after_bytes;  // drop link once this many image bytes received
    int32_t prn_report_offset = 0;                   // add to reported PRN counts
    bool reject_start_invalid_state = false;         // stale session: START -> 10 01 02
    bool respond_no_mem_at_packet = false;           // first data packet -> 10 03 06
    uint32_t max_app_size = 802816;
    bool require_settle = true;                      // drop size packet if no delay after START
    bool cancel = false;                             // ITransport::cancelled()
    std::optional<uint32_t> cancel_after_bytes;      // ...or once this many image bytes were received

    // ---- observable state ----
    bool connected = true;
    bool activated = false;
    bool erased = false;
    std::vector<uint8_t> received;
    std::vector<uint8_t> init;
    uint16_t prn = 0;
    uint32_t clock_ms = 0;
    size_t max_packet_len_seen = 0;

    bool write_control(std::span<const uint8_t> d) override {
        if (!connected || d.empty()) return false;
        switch (d[0]) {
            case 0x01:  // START
                if (d.size() < 2) return false;
                if (state_ != State::Idle || reject_start_invalid_state) {
                    notify({0x10, 0x01, 0x02});
                    return true;
                }
                state_ = State::AwaitSizes;
                delayed_since_start_ = false;
                return true;
            case 0x02:
                if (d.size() < 2) return false;
                if (d[1] == 0x00) {
                    state_ = State::InitCollect;
                    init.clear();
                } else if (state_ == State::InitCollect) {
                    dfu::InitPacket pkt;
                    const bool ok = dfu::parse_init_packet(init, pkt) == dfu::InitPacketError::Ok &&
                                    pkt.device_type == dfu::kAdafruitDeviceType;
                    if (ok) {
                        init_crc_ = pkt.firmware_crc16;
                        state_ = State::InitDone;
                    }
                    notify({0x10, 0x02, static_cast<uint8_t>(ok ? 0x01 : 0x06)});
                }
                return true;
            case 0x08:
                if (d.size() < 3) return false;
                prn = static_cast<uint16_t>(d[1] | (d[2] << 8));
                return true;
            case 0x03:
                if (state_ == State::InitDone) state_ = State::Receiving;
                return true;
            case 0x04: {
                const bool crc_ok = state_ == State::RxDone &&
                                    dfu::crc16_ccitt(received) == init_crc_;
                notify({0x10, 0x04, static_cast<uint8_t>(crc_ok ? 0x01 : 0x05)});
                if (crc_ok) state_ = State::Validated;
                return true;
            }
            case 0x05:
                if (state_ == State::Validated) activated = true;
                connected = false;  // resets without responding
                return false;
            case 0x06:
                connected = false;
                return false;
            default:
                return true;
        }
    }

    bool write_packet(std::span<const uint8_t> d) override {
        if (!connected) return false;
        max_packet_len_seen = std::max(max_packet_len_seen, d.size());
        switch (state_) {
            case State::AwaitSizes: {
                if (require_settle && !delayed_since_start_) return true;  // silently dropped
                if (d.size() != 12) {
                    notify({0x10, 0x01, 0x03});
                    return true;
                }
                const uint32_t sd = u32(d, 0), bl = u32(d, 4), app = u32(d, 8);
                if ((sd | bl | app) & 3u || sd || bl) {
                    notify({0x10, 0x01, 0x06});
                    return true;
                }
                if (app > max_app_size) {
                    notify({0x10, 0x01, 0x04});
                    state_ = State::Idle;
                    return true;
                }
                app_size_ = app;
                erased = true;
                state_ = State::Started;
                notify({0x10, 0x01, 0x01});
                return true;
            }
            case State::InitCollect:
                init.insert(init.end(), d.begin(), d.end());
                return true;
            case State::Receiving: {
                if (respond_no_mem_at_packet) {
                    notify({0x10, 0x03, 0x06});
                    return true;
                }
                if (d.size() % 4 != 0) {
                    notify({0x10, 0x03, 0x03});
                    return true;
                }
                received.insert(received.end(), d.begin(), d.end());
                ++packets_;
                if (disconnect_after_bytes && received.size() >= *disconnect_after_bytes) {
                    connected = false;
                    return true;
                }
                if (received.size() >= app_size_) {
                    state_ = State::RxDone;
                    if (prn && packets_ % prn == 0) notify_prn();  // real BL may send one here too
                    notify({0x10, 0x03, 0x01});
                } else if (prn && packets_ % prn == 0) {
                    notify_prn();
                }
                return true;
            }
            default:
                return true;  // ignored
        }
    }

    bool wait_notification(dfu::RawNotification& out, uint32_t timeout_ms) override {
        if (queue_.empty()) {
            clock_ms += timeout_ms;
            return false;
        }
        out = queue_.front();
        queue_.pop_front();
        return true;
    }

    void flush_notifications() override { queue_.clear(); }
    bool is_connected() const override { return connected; }
    bool cancelled() const override {
        return cancel || (cancel_after_bytes && received.size() >= *cancel_after_bytes);
    }
    void delay_ms(uint32_t ms) override {
        clock_ms += ms;
        delayed_since_start_ = true;
    }
    uint32_t now_ms() override { return clock_ms; }

private:
    enum class State { Idle, AwaitSizes, Started, InitCollect, InitDone, Receiving, RxDone, Validated };

    static uint32_t u32(std::span<const uint8_t> d, size_t o) {
        return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) |
               (static_cast<uint32_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
    }

    void notify(std::initializer_list<uint8_t> bytes) {
        dfu::RawNotification n;
        for (const uint8_t b : bytes) n.data[n.len++] = b;
        queue_.push_back(n);
    }

    void notify_prn() {
        const uint32_t v = static_cast<uint32_t>(static_cast<int64_t>(received.size()) + prn_report_offset);
        notify({0x11, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                static_cast<uint8_t>(v >> 24)});
    }

    State state_ = State::Idle;
    bool delayed_since_start_ = false;
    uint32_t app_size_ = 0;
    uint16_t init_crc_ = 0;
    uint32_t packets_ = 0;
    std::deque<dfu::RawNotification> queue_;
};

/// In-memory image source.
class VectorImage : public dfu::IImageSource {
public:
    explicit VectorImage(std::vector<uint8_t> d) : data(std::move(d)) {}
    bool read(uint32_t offset, std::span<uint8_t> out) override {
        if (fail_reads || offset + out.size() > data.size()) return false;
        std::copy_n(data.begin() + offset, out.size(), out.begin());
        return true;
    }
    std::vector<uint8_t> data;
    bool fail_reads = false;
};
