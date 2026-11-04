// SPDX-License-Identifier: MIT
//
// Nordic legacy BLE DFU (service 0x1530) as implemented by the Adafruit nRF52
// bootloader and its OTAFIX fork. Constants and wire encodings only; no I/O.
// See SPEC.md §6 and docs/PROTOCOL_NOTES.md for the verified sources.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dfu {

// ---- GATT --------------------------------------------------------------------

inline constexpr const char* kServiceUuid = "00001530-1212-efde-1523-785feabcd123";
inline constexpr const char* kControlPointUuid = "00001531-1212-efde-1523-785feabcd123";
inline constexpr const char* kPacketUuid = "00001532-1212-efde-1523-785feabcd123";
inline constexpr const char* kVersionUuid = "00001534-1212-efde-1523-785feabcd123";

/// Value of the Version characteristic (0x1534), little-endian u16.
inline constexpr uint16_t kVersionAppButtonless = 0x0001;  // Bluefruit BLEDfu in the app
inline constexpr uint16_t kVersionBootloader = 0x0008;     // Adafruit bootloader (SDK11)

// ---- Opcodes & status -------------------------------------------------------

enum class Op : uint8_t {
    StartDfu = 0x01,
    InitDfuParams = 0x02,
    ReceiveFirmwareImage = 0x03,
    ValidateFirmware = 0x04,
    ActivateAndReset = 0x05,
    SystemReset = 0x06,
    ReportReceivedImageSize = 0x07,
    PacketReceiptNotificationRequest = 0x08,
    Response = 0x10,
    PacketReceiptNotification = 0x11,
};

enum class ImageType : uint8_t {
    SoftDevice = 0x01,
    Bootloader = 0x02,
    SoftDeviceBootloader = 0x03,
    Application = 0x04,
};

enum class Status : uint8_t {
    Success = 0x01,
    InvalidState = 0x02,
    NotSupported = 0x03,
    DataSizeExceedsLimit = 0x04,
    CrcError = 0x05,
    OperationFailed = 0x06,
};

const char* to_string(Op op) noexcept;
const char* to_string(Status status) noexcept;

/// INIT_DFU_PARAMS sub-codes (second byte of the CP write).
inline constexpr uint8_t kInitParamsStart = 0x00;
inline constexpr uint8_t kInitParamsComplete = 0x01;

// ---- Control Point encoders -------------------------------------------------

/// `01 TT`: START DFU for the given image type. DFU32 only ever sends Application.
constexpr std::array<uint8_t, 2> encode_start(ImageType type = ImageType::Application) noexcept {
    return {static_cast<uint8_t>(Op::StartDfu), static_cast<uint8_t>(type)};
}

/// 12-byte size triplet written to the Packet characteristic after START:
/// `<u32 sd_size><u32 bl_size><u32 app_size>`, little-endian.
/// The bootloader rejects any other length and sizes that are not multiples of 4.
constexpr std::array<uint8_t, 12> encode_start_sizes(uint32_t app_size, uint32_t sd_size = 0,
                                                     uint32_t bl_size = 0) noexcept {
    std::array<uint8_t, 12> out{};
    const uint32_t sizes[3] = {sd_size, bl_size, app_size};
    for (size_t i = 0; i < 3; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>(sizes[i]);
        out[i * 4 + 1] = static_cast<uint8_t>(sizes[i] >> 8);
        out[i * 4 + 2] = static_cast<uint8_t>(sizes[i] >> 16);
        out[i * 4 + 3] = static_cast<uint8_t>(sizes[i] >> 24);
    }
    return out;
}

/// `02 00` / `02 01`.
constexpr std::array<uint8_t, 2> encode_init_params(bool complete) noexcept {
    return {static_cast<uint8_t>(Op::InitDfuParams),
            complete ? kInitParamsComplete : kInitParamsStart};
}

/// `08 NN NN`: request a packet receipt notification every `n` packets (0 disables).
constexpr std::array<uint8_t, 3> encode_prn_request(uint16_t n) noexcept {
    return {static_cast<uint8_t>(Op::PacketReceiptNotificationRequest), static_cast<uint8_t>(n),
            static_cast<uint8_t>(n >> 8)};
}

/// Single-byte opcodes: RECEIVE (03), VALIDATE (04), ACTIVATE (05), SYS_RESET (06), REPORT_SIZE (07).
constexpr std::array<uint8_t, 1> encode_op(Op op) noexcept {
    return {static_cast<uint8_t>(op)};
}

// ---- Control Point notification decoder -------------------------------------

struct Notification {
    enum class Kind : uint8_t {
        Invalid,
        Response,       // 10 <req_op> <status> [u32 for REPORT_SIZE]
        PacketReceipt,  // 11 <u32 bytes received>
    };
    Kind kind = Kind::Invalid;
    Op request = Op::Response;      // Response only
    Status status = Status::OperationFailed;  // Response only
    uint8_t raw_status = 0;         // Response only, as received
    uint32_t value = 0;             // PacketReceipt: bytes received; REPORT_SIZE response: size
    bool has_value = false;
};

/// Parses a Control Point notification. Malformed input yields Kind::Invalid.
/// Unknown status bytes are preserved in `raw_status`; `status` is then OperationFailed.
Notification parse_notification(std::span<const uint8_t> data) noexcept;

/// True if `n` is a Response to `op` with status Success.
constexpr bool is_success_for(const Notification& n, Op op) noexcept {
    return n.kind == Notification::Kind::Response && n.request == op && n.status == Status::Success;
}

}  // namespace dfu
