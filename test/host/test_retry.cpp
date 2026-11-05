// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "dfu_proto/retry.hpp"
#include "dfu_proto/transfer.hpp"
#include "fake_bootloader.hpp"

using dfu::AttemptOutcome;
using dfu::AttemptParams;
using dfu::FailureKind;
using dfu::TransferError;
using dfu::TransferPhase;

namespace {

dfu::TransferResult failed(TransferError e, TransferPhase p, uint8_t status = 0) {
    dfu::TransferResult r;
    r.error = e;
    r.phase = p;
    r.status = status;
    return r;
}

}  // namespace

TEST_CASE("Classifies transfer failures", "[retry]") {
    using dfu::classify_transfer_failure;
    CHECK(classify_transfer_failure(dfu::TransferResult{}) == FailureKind::None);

    // Bench: 7.5 ms interval overran OTAFIX -> link lost or 10 03 06 mid-stream.
    CHECK(classify_transfer_failure(failed(TransferError::Disconnected, TransferPhase::Stream)) ==
          FailureKind::LinkOrOverrun);
    CHECK(classify_transfer_failure(failed(TransferError::StatusError, TransferPhase::Stream, 0x06)) ==
          FailureKind::LinkOrOverrun);
    CHECK(classify_transfer_failure(failed(TransferError::PrnMismatch, TransferPhase::Stream)) ==
          FailureKind::LinkOrOverrun);
    CHECK(classify_transfer_failure(failed(TransferError::Timeout, TransferPhase::ReceiveComplete)) ==
          FailureKind::LinkOrOverrun);

    CHECK(classify_transfer_failure(failed(TransferError::StatusError, TransferPhase::Start, 0x02)) ==
          FailureKind::StaleSession);
    CHECK(classify_transfer_failure(failed(TransferError::StatusError, TransferPhase::Start, 0x04)) ==
          FailureKind::Fatal);
    CHECK(classify_transfer_failure(failed(TransferError::StatusError, TransferPhase::Init, 0x06)) ==
          FailureKind::Fatal);
    CHECK(classify_transfer_failure(failed(TransferError::StatusError, TransferPhase::Stream, 0x03)) ==
          FailureKind::Fatal);
    CHECK(classify_transfer_failure(failed(TransferError::BadParams, TransferPhase::Start)) == FailureKind::Fatal);
    CHECK(classify_transfer_failure(failed(TransferError::ReadFailed, TransferPhase::Stream)) == FailureKind::Fatal);

    CHECK(classify_transfer_failure(failed(TransferError::Disconnected, TransferPhase::Start)) ==
          FailureKind::Transient);
    CHECK(classify_transfer_failure(failed(TransferError::Timeout, TransferPhase::Init)) == FailureKind::Transient);
    CHECK(classify_transfer_failure(failed(TransferError::StatusError, TransferPhase::Validate, 0x05)) ==
          FailureKind::Transient);
}

TEST_CASE("Classifies failures produced by the simulated bootloader", "[retry]") {
    std::vector<uint8_t> img(244 * 40);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i);
    const uint16_t crc = dfu::crc16_ccitt(img);
    const std::vector<uint8_t> dat = {0x52, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 1, 0, 0xB6, 0,
                                      static_cast<uint8_t>(crc), static_cast<uint8_t>(crc >> 8)};
    dfu::TransferParams p;
    p.packet_size = 244;

    FakeBootloader no_mem;
    no_mem.respond_no_mem_at_packet = true;
    VectorImage src(img);
    CHECK(dfu::classify_transfer_failure(dfu::run_transfer(no_mem, src, static_cast<uint32_t>(img.size()), dat, p)) ==
          FailureKind::LinkOrOverrun);

    FakeBootloader stale;
    stale.reject_start_invalid_state = true;
    CHECK(dfu::classify_transfer_failure(dfu::run_transfer(stale, src, static_cast<uint32_t>(img.size()), dat, p)) ==
          FailureKind::StaleSession);
}

TEST_CASE("PRN cap", "[retry]") {
    CHECK(dfu::cap_prn(30) == 8);
    CHECK(dfu::cap_prn(16) == 8);
    CHECK(dfu::cap_prn(8) == 8);
    CHECK(dfu::cap_prn(4) == 4);
    CHECK(dfu::cap_prn(0) == 0);
}

TEST_CASE("Degrade: interval first, then PRN", "[retry]") {
    const dfu::RetryPolicy pol;
    AttemptParams p{8, 15};
    p = dfu::degrade(p, pol);
    CHECK(p.interval_ms == 30);
    CHECK(p.prn == 8);
    p = dfu::degrade(p, pol);
    CHECK(p.prn == 4);
    p = dfu::degrade(p, pol);
    CHECK(p.prn == 2);
    p = dfu::degrade(p, pol);
    CHECK(p.prn == 2);  // floor
    CHECK(p.interval_ms == 30);

    AttemptParams unpaced{0, 30};
    CHECK(dfu::degrade(unpaced, pol).prn == 8);
}

TEST_CASE("Retries until success, degrading after overruns", "[retry]") {
    std::vector<AttemptParams> seen;
    std::vector<uint32_t> sleeps;
    const auto r = dfu::run_with_retries(
        dfu::RetryPolicy{}, AttemptParams{8, 15},
        [&](uint8_t n, const AttemptParams& p) {
            seen.push_back(p);
            return n < 3 ? AttemptOutcome{false, FailureKind::LinkOrOverrun} : AttemptOutcome{true, FailureKind::None};
        },
        [&](uint32_t ms) { sleeps.push_back(ms); });
    CHECK(r.last.ok);
    CHECK(r.attempts == 3);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0].interval_ms == 15);
    CHECK(seen[1].interval_ms == 30);
    CHECK(seen[1].prn == 8);
    CHECK(seen[2].prn == 4);
    CHECK(sleeps == std::vector<uint32_t>{3000, 6000});
}

TEST_CASE("Transient failures retry with unchanged parameters", "[retry]") {
    std::vector<AttemptParams> seen;
    const auto r = dfu::run_with_retries(
        dfu::RetryPolicy{}, AttemptParams{8, 20},
        [&](uint8_t, const AttemptParams& p) {
            seen.push_back(p);
            return AttemptOutcome{false, FailureKind::Transient};
        },
        [](uint32_t) {});
    CHECK_FALSE(r.last.ok);
    CHECK(r.attempts == 3);
    for (const auto& p : seen) {
        CHECK(p.interval_ms == 20);
        CHECK(p.prn == 8);
    }
}

TEST_CASE("Fatal and target-gone stop immediately", "[retry]") {
    for (FailureKind stop : {FailureKind::Fatal, FailureKind::TargetGone}) {
        int calls = 0;
        int slept = 0;
        const auto r = dfu::run_with_retries(
            dfu::RetryPolicy{}, AttemptParams{},
            [&](uint8_t, const AttemptParams&) {
                ++calls;
                return AttemptOutcome{false, stop};
            },
            [&](uint32_t) { ++slept; });
        CHECK(calls == 1);
        CHECK(slept == 0);
        CHECK(r.last.failure == stop);
    }
}

TEST_CASE("Stale session retries after the short delay", "[retry]") {
    std::vector<uint32_t> sleeps;
    const auto r = dfu::run_with_retries(
        dfu::RetryPolicy{}, AttemptParams{},
        [&](uint8_t n, const AttemptParams&) {
            return n == 1 ? AttemptOutcome{false, FailureKind::StaleSession} : AttemptOutcome{true, FailureKind::None};
        },
        [&](uint32_t ms) { sleeps.push_back(ms); });
    CHECK(r.last.ok);
    CHECK(sleeps == std::vector<uint32_t>{1000});
}

TEST_CASE("Attempt budget of one means no retry", "[retry]") {
    dfu::RetryPolicy pol;
    pol.max_attempts = 1;
    int calls = 0;
    const auto r = dfu::run_with_retries(
        pol, AttemptParams{},
        [&](uint8_t, const AttemptParams&) {
            ++calls;
            return AttemptOutcome{false, FailureKind::Transient};
        },
        [](uint32_t) { FAIL("should not sleep"); });
    CHECK(calls == 1);
    CHECK(r.attempts == 1);
}
