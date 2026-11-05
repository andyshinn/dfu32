// SPDX-License-Identifier: MIT
//
// Retry policy for flashing (SPEC.md §6.5). Pure logic, host-tested:
//  - classify why an attempt failed,
//  - decide whether to try again and after how long,
//  - make the next attempt more conservative after a stream overrun or link
//    loss (bench: fast connection intervals overrun OTAFIX's lazy-erase
//    buffering; see docs/BENCH.md).
#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "dfu_proto/bootloader_info.hpp"
#include "dfu_proto/transfer.hpp"

namespace dfu {

enum class FailureKind : uint8_t {
    None,
    Transient,      // retry with the same parameters (connect failed, timeout outside the stream, ...)
    LinkOrOverrun,  // link lost / 06 / receipt problem while streaming: retry, slower
    StaleSession,   // START answered INVALID_STATE: SYS_RESET was sent, retry promptly
    Fatal,          // retrying cannot help (image/target mismatch, image too large, read error)
    TargetGone,     // target no longer reachable over BLE (e.g. old bootloader fell back to USB)
};

const char* to_string(FailureKind k) noexcept;

/// Classifies a failed transfer attempt. Ok -> None.
FailureKind classify_transfer_failure(const TransferResult& r) noexcept;

/// PRN above this overruns OTAFIX-class bootloaders when the link is fast:
/// receipts acknowledge arrival, not flash writes, and the lazy-erase path
/// buffers only 8 packets (ESPHome's reference client notes the same limit).
inline constexpr uint16_t kMaxSafePrn = 8;

/// Requested PRN limited to kMaxSafePrn for every bootloader class (0 = off is
/// left alone: it is a deliberate choice, not a window size).
uint16_t cap_prn(uint16_t requested) noexcept;

struct AttemptParams {
    uint16_t prn = 8;
    uint16_t interval_ms = 20;
};

struct RetryPolicy {
    uint8_t max_attempts = 3;
    std::array<uint32_t, 3> backoff_ms{3000, 6000, 12000};  // after attempt 1, 2, 3+
    uint32_t stale_session_delay_ms = 1000;                 // bootloader reboots after SYS_RESET
    uint16_t slowest_interval_ms = 30;
    uint16_t min_prn = 2;
};

/// One step more conservative: lengthen the connection interval up to the
/// policy's slowest (20 -> 30 ms), then halve PRN down to min_prn. Returns the
/// input unchanged once both are at their limits.
AttemptParams degrade(const AttemptParams& p, const RetryPolicy& policy) noexcept;

struct AttemptOutcome {
    bool ok = false;
    FailureKind failure = FailureKind::None;
};

struct RetryResult {
    AttemptOutcome last;
    uint8_t attempts = 0;
    AttemptParams last_params;
};

using AttemptFn = std::function<AttemptOutcome(uint8_t attempt, const AttemptParams& params)>;
using SleepFn = std::function<void(uint32_t ms)>;

/// Runs attempts until one succeeds, a failure is Fatal/TargetGone, or the
/// policy's attempt budget is spent. Sleeps between attempts (backoff, or the
/// short stale-session delay) and degrades parameters after LinkOrOverrun.
RetryResult run_with_retries(const RetryPolicy& policy, AttemptParams initial, const AttemptFn& attempt,
                             const SleepFn& sleep);

}  // namespace dfu
