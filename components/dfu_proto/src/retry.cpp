// SPDX-License-Identifier: MIT
#include "dfu_proto/retry.hpp"

#include <algorithm>

namespace dfu {

const char* to_string(FailureKind k) noexcept {
    switch (k) {
        case FailureKind::None: return "NONE";
        case FailureKind::Transient: return "TRANSIENT";
        case FailureKind::LinkOrOverrun: return "LINK_OR_OVERRUN";
        case FailureKind::StaleSession: return "STALE_SESSION";
        case FailureKind::Fatal: return "FATAL";
        case FailureKind::TargetGone: return "TARGET_GONE";
    }
    return "UNKNOWN";
}

FailureKind classify_transfer_failure(const TransferResult& r) noexcept {
    const bool streaming = r.phase == TransferPhase::Stream || r.phase == TransferPhase::ReceiveComplete;
    switch (r.error) {
        case TransferError::Ok:
            return FailureKind::None;
        case TransferError::BadParams:
        case TransferError::ReadFailed:
            return FailureKind::Fatal;
        case TransferError::PrnMismatch:
            return FailureKind::LinkOrOverrun;
        case TransferError::Disconnected:
        case TransferError::WriteFailed:
        case TransferError::Timeout:
            return streaming ? FailureKind::LinkOrOverrun : FailureKind::Transient;
        case TransferError::UnexpectedNotification:
            return FailureKind::Transient;
        case TransferError::StatusError:
            break;
    }

    const auto status = static_cast<Status>(r.status);
    switch (r.phase) {
        case TransferPhase::Start:
            if (status == Status::InvalidState) return FailureKind::StaleSession;
            if (status == Status::DataSizeExceedsLimit) return FailureKind::Fatal;
            if (status == Status::NotSupported) return FailureKind::Fatal;  // sizes rejected
            return FailureKind::Transient;
        case TransferPhase::Init:
            // OPERATION_FAILED here means the bootloader rejected the init packet
            // (device type / SoftDevice); resending the same .dat cannot help.
            return status == Status::OperationFailed ? FailureKind::Fatal : FailureKind::Transient;
        case TransferPhase::Stream:
        case TransferPhase::ReceiveComplete:
            // 03 = a packet length the bootloader refuses: our bug, not the link.
            return status == Status::NotSupported ? FailureKind::Fatal : FailureKind::LinkOrOverrun;
        default:
            return FailureKind::Transient;  // e.g. CRC error at VALIDATE: data damaged in transit
    }
}

uint16_t cap_prn(uint16_t requested) noexcept { return std::min(requested, kMaxSafePrn); }

AttemptParams degrade(const AttemptParams& p, const RetryPolicy& policy) noexcept {
    AttemptParams next = p;
    if (next.interval_ms < policy.slowest_interval_ms) {
        next.interval_ms = policy.slowest_interval_ms;
    } else if (next.prn > policy.min_prn) {
        next.prn = std::max<uint16_t>(policy.min_prn, static_cast<uint16_t>(next.prn / 2));
    } else if (next.prn == 0) {
        next.prn = kMaxSafePrn;  // unpaced streaming failed: start pacing
    }
    return next;
}

RetryResult run_with_retries(const RetryPolicy& policy, AttemptParams initial, const AttemptFn& attempt,
                             const SleepFn& sleep) {
    RetryResult result;
    AttemptParams params = initial;
    const uint8_t max_attempts = std::max<uint8_t>(1, policy.max_attempts);
    for (uint8_t n = 1; n <= max_attempts; ++n) {
        result.attempts = n;
        result.last_params = params;
        result.last = attempt(n, params);
        if (result.last.ok || result.last.failure == FailureKind::Fatal ||
            result.last.failure == FailureKind::TargetGone || n == max_attempts) {
            break;
        }
        const size_t idx = std::min<size_t>(n - 1, policy.backoff_ms.size() - 1);
        sleep(result.last.failure == FailureKind::StaleSession ? policy.stale_session_delay_ms
                                                               : policy.backoff_ms[idx]);
        if (result.last.failure == FailureKind::LinkOrOverrun) {
            params = degrade(params, policy);
        }
    }
    return result;
}

}  // namespace dfu
