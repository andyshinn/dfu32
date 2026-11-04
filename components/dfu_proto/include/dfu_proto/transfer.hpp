// SPDX-License-Identifier: MIT
//
// One legacy-DFU transfer attempt against a bootloader that is already
// connected (SPEC.md §6.2–§6.4): START → INIT → PRN → RECEIVE → stream →
// VALIDATE → ACTIVATE. Transport-agnostic so it can be tested on the host
// against a simulated bootloader; the ESP32 implementation lives in dfu_ble.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>

#include "dfu_proto/protocol.hpp"

namespace dfu {

struct RawNotification {
    std::array<uint8_t, 20> data{};
    uint8_t len = 0;
    std::span<const uint8_t> bytes() const { return {data.data(), len}; }
};

/// Blocking link to the bootloader's DFU service. The caller has already
/// connected, discovered the service and enabled Control Point notifications.
class ITransport {
public:
    virtual ~ITransport() = default;
    /// Control Point write *with* response. False on ATT failure or disconnect.
    virtual bool write_control(std::span<const uint8_t> data) = 0;
    /// Packet write *without* response. Implementations absorb local
    /// back-pressure (e.g. NimBLE out of mbufs) and only fail on real errors.
    virtual bool write_packet(std::span<const uint8_t> data) = 0;
    /// Waits up to `timeout_ms` for the next Control Point notification.
    virtual bool wait_notification(RawNotification& out, uint32_t timeout_ms) = 0;
    /// Drops any queued notifications.
    virtual void flush_notifications() = 0;
    virtual bool is_connected() const = 0;
    virtual void delay_ms(uint32_t ms) = 0;
    virtual uint32_t now_ms() = 0;
};

/// Random-access reader for the application image.
class IImageSource {
public:
    virtual ~IImageSource() = default;
    virtual bool read(uint32_t offset, std::span<uint8_t> out) = 0;
};

struct TransferParams {
    uint16_t packet_size = 20;         // from packet_size_for_mtu(); multiple of 4
    uint16_t prn = 8;                  // packets per receipt notification; 0 = off
    uint32_t start_settle_ms = 500;    // START CP write → size packet (dropped if too early)
    uint32_t start_timeout_ms = 60000; // upstream erases the whole image before replying
    uint32_t init_timeout_ms = 30000;
    uint32_t prn_timeout_ms = 5000;
    uint32_t receive_timeout_ms = 60000;  // after the last byte
    uint32_t validate_timeout_ms = 30000;
    uint32_t activate_disconnect_ms = 10000;
};

enum class TransferPhase : uint8_t {
    Start,
    Init,
    PrnRequest,
    Receive,
    Stream,
    ReceiveComplete,
    Validate,
    Activate,
    Done,
};
inline constexpr size_t kTransferPhaseCount = static_cast<size_t>(TransferPhase::Done) + 1;

enum class TransferError : uint8_t {
    Ok,
    BadParams,
    WriteFailed,
    Disconnected,
    Timeout,
    StatusError,        // bootloader answered with a non-SUCCESS status
    PrnMismatch,        // receipt byte count != bytes sent
    UnexpectedNotification,
    ReadFailed,         // image source failed
};

const char* to_string(TransferPhase p) noexcept;
const char* to_string(TransferError e) noexcept;

struct TransferResult {
    TransferError error = TransferError::Ok;
    TransferPhase phase = TransferPhase::Start;  // phase reached (failed in, or Done)
    uint8_t status = 0;                          // raw status for StatusError
    uint32_t bytes_sent = 0;
    uint32_t bytes_acked = 0;                    // per last receipt / completion
    bool target_erased = false;                  // START size packet was written
    bool activate_disconnect_seen = false;
    std::array<uint32_t, kTransferPhaseCount> phase_ms{};

    bool ok() const { return error == TransferError::Ok; }
};

using ProgressFn = std::function<void(uint32_t bytes_acked, uint32_t total)>;

/// Runs one attempt. Does not retry and does not send SYS_RESET; recovery is
/// the caller's job (SPEC.md §6.5).
TransferResult run_transfer(ITransport& link, IImageSource& image, uint32_t image_size,
                            std::span<const uint8_t> init_packet, const TransferParams& params,
                            const ProgressFn& progress = {});

}  // namespace dfu
