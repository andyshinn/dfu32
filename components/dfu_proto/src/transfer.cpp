// SPDX-License-Identifier: MIT
#include "dfu_proto/transfer.hpp"

#include <vector>

#include "dfu_proto/chunk.hpp"

namespace dfu {

const char* to_string(TransferPhase p) noexcept {
    switch (p) {
        case TransferPhase::Start: return "START";
        case TransferPhase::Init: return "INIT";
        case TransferPhase::PrnRequest: return "PRN_REQUEST";
        case TransferPhase::Receive: return "RECEIVE";
        case TransferPhase::Stream: return "STREAM";
        case TransferPhase::ReceiveComplete: return "RECEIVE_COMPLETE";
        case TransferPhase::Validate: return "VALIDATE";
        case TransferPhase::Activate: return "ACTIVATE";
        case TransferPhase::Done: return "DONE";
    }
    return "UNKNOWN";
}

const char* to_string(TransferError e) noexcept {
    switch (e) {
        case TransferError::Ok: return "OK";
        case TransferError::BadParams: return "BAD_PARAMS";
        case TransferError::WriteFailed: return "WRITE_FAILED";
        case TransferError::Disconnected: return "LINK_LOST";
        case TransferError::Timeout: return "RESPONSE_TIMEOUT";
        case TransferError::StatusError: return "DFU_STATUS";
        case TransferError::PrnMismatch: return "PRN_MISMATCH";
        case TransferError::UnexpectedNotification: return "UNEXPECTED_NOTIFICATION";
        case TransferError::ReadFailed: return "IMAGE_READ_FAILED";
    }
    return "UNKNOWN";
}

namespace {

class Runner {
public:
    Runner(ITransport& link, TransferResult& r) : link_(link), r_(r) {}

    void enter(TransferPhase p) {
        const uint32_t now = link_.now_ms();
        r_.phase_ms[static_cast<size_t>(r_.phase)] += now - phase_start_;
        r_.phase = p;
        phase_start_ = now;
    }

    void begin() { phase_start_ = link_.now_ms(); }

    bool fail(TransferError e, uint8_t status = 0) {
        r_.error = e;
        r_.status = status;
        enter(r_.phase);  // account time spent in the failing phase
        return false;
    }

    TransferError link_error() const {
        return link_.is_connected() ? TransferError::WriteFailed : TransferError::Disconnected;
    }

    bool control(std::span<const uint8_t> d) {
        return link_.write_control(d) || fail(link_error());
    }

    bool packet(std::span<const uint8_t> d) {
        return link_.write_packet(d) || fail(link_error());
    }

    /// Waits for the Response to `op`, skipping packet receipts.
    bool response(Op op, uint32_t timeout_ms) {
        const uint32_t deadline = link_.now_ms() + timeout_ms;
        for (;;) {
            const uint32_t now = link_.now_ms();
            if (static_cast<int32_t>(deadline - now) <= 0) {
                return fail(link_.is_connected() ? TransferError::Timeout : TransferError::Disconnected);
            }
            RawNotification raw;
            if (!link_.wait_notification(raw, deadline - now)) {
                if (!link_.is_connected()) {
                    return fail(TransferError::Disconnected);
                }
                continue;  // re-evaluate deadline
            }
            const Notification n = parse_notification(raw.bytes());
            if (n.kind == Notification::Kind::PacketReceipt) {
                r_.bytes_acked = n.value;
                continue;
            }
            if (n.kind != Notification::Kind::Response || n.request != op) {
                return fail(TransferError::UnexpectedNotification);
            }
            if (n.status != Status::Success) {
                return fail(TransferError::StatusError, n.raw_status);
            }
            return true;
        }
    }

    /// Waits for a packet receipt reporting exactly `expected` bytes.
    bool receipt(uint32_t expected, uint32_t timeout_ms) {
        const uint32_t deadline = link_.now_ms() + timeout_ms;
        for (;;) {
            const uint32_t now = link_.now_ms();
            if (static_cast<int32_t>(deadline - now) <= 0) {
                return fail(link_.is_connected() ? TransferError::Timeout : TransferError::Disconnected);
            }
            RawNotification raw;
            if (!link_.wait_notification(raw, deadline - now)) {
                if (!link_.is_connected()) {
                    return fail(TransferError::Disconnected);
                }
                continue;
            }
            const Notification n = parse_notification(raw.bytes());
            if (n.kind == Notification::Kind::PacketReceipt) {
                r_.bytes_acked = n.value;
                return n.value == expected || fail(TransferError::PrnMismatch);
            }
            if (n.kind == Notification::Kind::Response) {
                // e.g. 10 03 06 (bootloader out of buffers) or 10 03 03 (bad length)
                return fail(n.status == Status::Success ? TransferError::UnexpectedNotification
                                                        : TransferError::StatusError,
                            n.raw_status);
            }
            return fail(TransferError::UnexpectedNotification);
        }
    }

private:
    ITransport& link_;
    TransferResult& r_;
    uint32_t phase_start_ = 0;
};

}  // namespace

TransferResult run_transfer(ITransport& link, IImageSource& image, uint32_t image_size,
                            std::span<const uint8_t> init_packet, const TransferParams& params,
                            const ProgressFn& progress) {
    TransferResult r;
    Runner run(link, r);
    run.begin();

    ChunkPlan plan;
    if (!plan_chunks(image_size, params.packet_size, plan) || init_packet.empty() ||
        init_packet.size() > params.packet_size) {
        run.fail(TransferError::BadParams);
        return r;
    }

    link.flush_notifications();

    // START: CP 01 04, settle, 12-byte size triplet on Packet, wait for erase.
    run.enter(TransferPhase::Start);
    if (!run.control(encode_start(ImageType::Application))) return r;
    link.delay_ms(params.start_settle_ms);
    const auto sizes = encode_start_sizes(image_size);
    if (!run.packet(sizes)) return r;
    r.target_erased = true;
    if (!run.response(Op::StartDfu, params.start_timeout_ms)) return r;

    // INIT: 02 00, whole .dat in one Packet write, 02 01.
    run.enter(TransferPhase::Init);
    if (!run.control(encode_init_params(false))) return r;
    if (!run.packet(init_packet)) return r;
    if (!run.control(encode_init_params(true))) return r;
    if (!run.response(Op::InitDfuParams, params.init_timeout_ms)) return r;

    run.enter(TransferPhase::PrnRequest);
    if (!run.control(encode_prn_request(params.prn))) return r;

    run.enter(TransferPhase::Receive);
    if (!run.control(encode_op(Op::ReceiveFirmwareImage))) return r;

    run.enter(TransferPhase::Stream);
    std::vector<uint8_t> buf(params.packet_size);
    uint32_t offset = 0;
    for (uint32_t i = 0; i < plan.packet_count; ++i) {
        const bool last = (i + 1 == plan.packet_count);
        const uint16_t len = last ? plan.last_packet_size : plan.packet_size;
        const std::span<uint8_t> chunk(buf.data(), len);
        if (!image.read(offset, chunk)) {
            run.fail(TransferError::ReadFailed);
            return r;
        }
        if (!run.packet(chunk)) return r;
        offset += len;
        r.bytes_sent = offset;

        if (params.prn != 0 && !last && ((i + 1) % params.prn) == 0) {
            if (!run.receipt(offset, params.prn_timeout_ms)) return r;
            if (progress) progress(r.bytes_acked, image_size);
        } else if (params.prn == 0 && (i % 64) == 63) {
            link.delay_ms(1);  // let lower-priority tasks run when unpaced
        }
    }

    run.enter(TransferPhase::ReceiveComplete);
    if (!run.response(Op::ReceiveFirmwareImage, params.receive_timeout_ms)) return r;
    r.bytes_acked = image_size;
    if (progress) progress(image_size, image_size);

    run.enter(TransferPhase::Validate);
    if (!run.control(encode_op(Op::ValidateFirmware))) return r;
    if (!run.response(Op::ValidateFirmware, params.validate_timeout_ms)) return r;

    // ACTIVATE: the bootloader resets without answering; a failed write or a
    // disconnect here is the expected outcome.
    run.enter(TransferPhase::Activate);
    (void)link.write_control(encode_op(Op::ActivateAndReset));
    const uint32_t deadline = link.now_ms() + params.activate_disconnect_ms;
    while (link.is_connected() && static_cast<int32_t>(deadline - link.now_ms()) > 0) {
        link.delay_ms(50);
    }
    r.activate_disconnect_seen = !link.is_connected();

    run.enter(TransferPhase::Done);
    return r;
}

}  // namespace dfu
