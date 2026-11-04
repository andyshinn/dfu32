// SPDX-License-Identifier: MIT
#include "dfu_proto/protocol.hpp"

namespace dfu {
namespace {

uint32_t read_u32le(std::span<const uint8_t> d, size_t off) noexcept {
    return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

bool is_known_status(uint8_t s) noexcept {
    return s >= static_cast<uint8_t>(Status::Success) &&
           s <= static_cast<uint8_t>(Status::OperationFailed);
}

bool is_request_op(uint8_t op) noexcept {
    return op >= static_cast<uint8_t>(Op::StartDfu) &&
           op <= static_cast<uint8_t>(Op::PacketReceiptNotificationRequest);
}

}  // namespace

const char* to_string(Op op) noexcept {
    switch (op) {
        case Op::StartDfu: return "START_DFU";
        case Op::InitDfuParams: return "INIT_DFU_PARAMS";
        case Op::ReceiveFirmwareImage: return "RECEIVE_FIRMWARE_IMAGE";
        case Op::ValidateFirmware: return "VALIDATE_FIRMWARE";
        case Op::ActivateAndReset: return "ACTIVATE_AND_RESET";
        case Op::SystemReset: return "SYSTEM_RESET";
        case Op::ReportReceivedImageSize: return "REPORT_RECEIVED_IMAGE_SIZE";
        case Op::PacketReceiptNotificationRequest: return "PRN_REQUEST";
        case Op::Response: return "RESPONSE";
        case Op::PacketReceiptNotification: return "PRN";
    }
    return "UNKNOWN_OP";
}

const char* to_string(Status status) noexcept {
    switch (status) {
        case Status::Success: return "SUCCESS";
        case Status::InvalidState: return "INVALID_STATE";
        case Status::NotSupported: return "NOT_SUPPORTED";
        case Status::DataSizeExceedsLimit: return "DATA_SIZE_EXCEEDS_LIMIT";
        case Status::CrcError: return "CRC_ERROR";
        case Status::OperationFailed: return "OPERATION_FAILED";
    }
    return "UNKNOWN_STATUS";
}

Notification parse_notification(std::span<const uint8_t> data) noexcept {
    Notification n;
    if (data.empty()) {
        return n;
    }
    switch (static_cast<Op>(data[0])) {
        case Op::Response: {
            if (data.size() < 3 || !is_request_op(data[1])) {
                return n;
            }
            n.request = static_cast<Op>(data[1]);
            n.raw_status = data[2];
            n.status = is_known_status(data[2]) ? static_cast<Status>(data[2]) : Status::OperationFailed;
            if (n.request == Op::ReportReceivedImageSize && n.status == Status::Success) {
                if (data.size() < 7) {
                    return n;  // kind stays Invalid
                }
                n.value = read_u32le(data, 3);
                n.has_value = true;
            }
            n.kind = Notification::Kind::Response;
            return n;
        }
        case Op::PacketReceiptNotification: {
            if (data.size() < 5) {
                return n;
            }
            n.value = read_u32le(data, 1);
            n.has_value = true;
            n.kind = Notification::Kind::PacketReceipt;
            return n;
        }
        default:
            return n;
    }
}

}  // namespace dfu
