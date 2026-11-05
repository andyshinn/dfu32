// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "dfu_proto/crc16.hpp"
#include "dfu_proto/transfer.hpp"
#include "fake_bootloader.hpp"

using dfu::TransferError;
using dfu::TransferParams;
using dfu::TransferPhase;

namespace {

std::vector<uint8_t> make_image(size_t size) {
    std::vector<uint8_t> img(size);
    for (size_t i = 0; i < size; ++i) img[i] = static_cast<uint8_t>((i * 7 + 3) ^ (i >> 8));
    return img;
}

std::vector<uint8_t> make_dat(const std::vector<uint8_t>& img, uint16_t device_type = 0x0052) {
    const uint16_t crc = dfu::crc16_ccitt(img);
    return {static_cast<uint8_t>(device_type), static_cast<uint8_t>(device_type >> 8), 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0xB6, 0x00, static_cast<uint8_t>(crc),
            static_cast<uint8_t>(crc >> 8)};
}

TransferParams params(uint16_t packet, uint16_t prn = 8) {
    TransferParams p;
    p.packet_size = packet;
    p.prn = prn;
    return p;
}

}  // namespace

TEST_CASE("Happy path with 244-byte packets and PRN 8", "[transfer]") {
    const auto img = make_image(440924);  // RAK4631 repeater size, last packet 16 bytes
    const auto dat = make_dat(img);
    FakeBootloader bl;
    VectorImage src(img);
    uint32_t last_progress = 0;
    int progress_calls = 0;

    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), dat, params(244),
                                     [&](uint32_t acked, uint32_t total) {
                                         CHECK(acked >= last_progress);
                                         CHECK(total == img.size());
                                         last_progress = acked;
                                         ++progress_calls;
                                     });
    INFO(dfu::to_string(r.error) << " in " << dfu::to_string(r.phase));
    REQUIRE(r.ok());
    CHECK(r.phase == TransferPhase::Done);
    CHECK(bl.activated);
    CHECK(bl.received == img);
    CHECK(bl.init == dat);
    CHECK(bl.prn == 8);
    CHECK(bl.max_packet_len_seen == 244);
    CHECK(r.bytes_sent == img.size());
    CHECK(r.bytes_acked == img.size());
    CHECK(r.target_erased);
    CHECK(r.activate_disconnect_seen);
    CHECK(last_progress == img.size());
    CHECK(progress_calls > 200);
}

TEST_CASE("Happy path with legacy 20-byte packets", "[transfer]") {
    const auto img = make_image(4096 + 8);
    FakeBootloader bl;
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(20, 10));
    REQUIRE(r.ok());
    CHECK(bl.received == img);
    CHECK(bl.max_packet_len_seen == 20);
}

TEST_CASE("Image whose packet count is a multiple of PRN", "[transfer]") {
    // 16 packets of 244 with PRN 8: bootloader sends a receipt with the RX-complete
    // response; the client must skip it while waiting for 10 03 01.
    const auto img = make_image(244 * 16);
    FakeBootloader bl;
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(244));
    REQUIRE(r.ok());
    CHECK(bl.activated);
}

TEST_CASE("PRN disabled", "[transfer]") {
    const auto img = make_image(244 * 100 + 4);
    FakeBootloader bl;
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(244, 0));
    REQUIRE(r.ok());
    CHECK(bl.received == img);
}

TEST_CASE("Without the settle delay START never answers", "[transfer]") {
    const auto img = make_image(1024);
    VectorImage src(img);
    auto p = params(244);
    p.start_settle_ms = 0;
    // The fake treats any delay_ms() after START as "settled"; model a client
    // racing ahead by never marking it settled.
    struct NoDelay : FakeBootloader {
        void delay_ms(uint32_t ms) override { clock_ms += ms; }  // never marks settled
    } racy;
    const auto r = dfu::run_transfer(racy, src, static_cast<uint32_t>(img.size()), make_dat(img), p);
    CHECK(r.error == TransferError::Timeout);
    CHECK(r.phase == TransferPhase::Start);
    CHECK(r.target_erased);  // we can't know whether the erase began
}

TEST_CASE("Stale session: START returns INVALID_STATE", "[transfer]") {
    const auto img = make_image(1024);
    FakeBootloader bl;
    bl.reject_start_invalid_state = true;
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(244));
    CHECK(r.error == TransferError::StatusError);
    CHECK(r.status == 0x02);
    CHECK(r.phase == TransferPhase::Start);
}

TEST_CASE("Image too large for the bootloader", "[transfer]") {
    const auto img = make_image(2048);
    FakeBootloader bl;
    bl.max_app_size = 1024;
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(244));
    CHECK(r.error == TransferError::StatusError);
    CHECK(r.status == 0x04);
    CHECK_FALSE(bl.erased);
}

TEST_CASE("INIT rejected for wrong device type", "[transfer]") {
    const auto img = make_image(1024);
    FakeBootloader bl;
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img, 0xFFFF), params(244));
    CHECK(r.error == TransferError::StatusError);
    CHECK(r.status == 0x06);
    CHECK(r.phase == TransferPhase::Init);
    CHECK(bl.erased);  // erase already happened at START
}

TEST_CASE("Link drops mid-stream", "[transfer]") {
    const auto img = make_image(244 * 100);
    FakeBootloader bl;
    bl.disconnect_after_bytes = 244 * 37;
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(244));
    CHECK(r.error == TransferError::Disconnected);
    CHECK(r.phase == TransferPhase::Stream);
    CHECK(r.target_erased);
    CHECK(r.bytes_acked <= r.bytes_sent);
}

TEST_CASE("Receipt byte count mismatch is detected", "[transfer]") {
    const auto img = make_image(244 * 40);
    FakeBootloader bl;
    bl.prn_report_offset = -4;
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(244));
    CHECK(r.error == TransferError::PrnMismatch);
    CHECK(r.phase == TransferPhase::Stream);
}

TEST_CASE("Bootloader error during streaming", "[transfer]") {
    const auto img = make_image(244 * 40);
    FakeBootloader bl;
    bl.respond_no_mem_at_packet = true;
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(244));
    CHECK(r.error == TransferError::StatusError);
    CHECK(r.status == 0x06);
}

TEST_CASE("CRC mismatch at VALIDATE", "[transfer]") {
    const auto img = make_image(244 * 10);
    auto dat = make_dat(img);
    dat[12] ^= 0x01;  // corrupt CRC in init packet
    FakeBootloader bl;
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), dat, params(244));
    CHECK(r.error == TransferError::StatusError);
    CHECK(r.status == 0x05);
    CHECK(r.phase == TransferPhase::Validate);
    CHECK_FALSE(bl.activated);
}

TEST_CASE("Bad parameters are rejected before touching the target", "[transfer]") {
    const auto img = make_image(1024);
    const auto dat = make_dat(img);
    FakeBootloader bl;
    VectorImage src(img);
    CHECK(dfu::run_transfer(bl, src, 1022, dat, params(244)).error == TransferError::BadParams);
    CHECK(dfu::run_transfer(bl, src, 1024, dat, params(182)).error == TransferError::BadParams);
    CHECK(dfu::run_transfer(bl, src, 1024, {}, params(244)).error == TransferError::BadParams);
    CHECK_FALSE(bl.erased);
}

TEST_CASE("Image read failure aborts", "[transfer]") {
    const auto img = make_image(1024);
    FakeBootloader bl;
    VectorImage src(img);
    src.fail_reads = true;
    const auto r = dfu::run_transfer(bl, src, 1024, make_dat(img), params(244));
    CHECK(r.error == TransferError::ReadFailed);
    CHECK(r.phase == TransferPhase::Stream);
}

TEST_CASE("Disconnect during a receipt wait is reported immediately", "[transfer]") {
    // Models the BLE transport's disconnect wake-up: wait_notification returns
    // false at once and is_connected() is false. The engine must report
    // LINK_LOST without waiting out the receipt timeout (review finding).
    struct Vanishing : FakeBootloader {
        bool vanished = false;
        bool wait_notification(dfu::RawNotification& out, uint32_t timeout_ms) override {
            if (!vanished && received.size() >= 244 * 16) {
                vanished = true;
                connected = false;
                return false;  // no clock advance: woken, not timed out
            }
            return FakeBootloader::wait_notification(out, timeout_ms);
        }
    } bl;
    const auto img = make_image(244 * 64);
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(244));
    CHECK(r.error == TransferError::Disconnected);
    CHECK(r.phase == TransferPhase::Stream);
    CHECK(bl.clock_ms < 1000);  // settle delay only; no 5 s receipt timeout
}

TEST_CASE("Abort before START leaves the target untouched", "[transfer][abort]") {
    const auto img = make_image(244 * 20);
    FakeBootloader bl;
    bl.cancel = true;
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(244));
    CHECK(r.error == TransferError::Aborted);
    CHECK(r.phase == TransferPhase::Start);
    CHECK_FALSE(r.target_erased);
    CHECK_FALSE(bl.erased);
    CHECK(r.bytes_sent == 0);
}

TEST_CASE("Abort while waiting for START stops without waiting out the erase", "[transfer][abort]") {
    // The real transport's wait_notification() returns early once cancelled.
    struct Hanging : FakeBootloader {
        bool wait_notification(dfu::RawNotification& out, uint32_t timeout_ms) override {
            if (erased) {  // START accepted: pretend the erase takes a long time
                cancel = true;
                clock_ms += 100;
                return false;
            }
            return FakeBootloader::wait_notification(out, timeout_ms);
        }
    } bl;
    const auto img = make_image(244 * 20);
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(244));
    CHECK(r.error == TransferError::Aborted);
    CHECK(r.phase == TransferPhase::Start);
    CHECK(r.target_erased);
    CHECK(bl.clock_ms < 1000);  // settle + one poll, not the 60 s START timeout
}

TEST_CASE("Abort mid-stream stops before the next packet", "[transfer][abort]") {
    const auto img = make_image(244 * 100);
    FakeBootloader bl;
    bl.cancel_after_bytes = 244 * 37;
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(244));
    CHECK(r.error == TransferError::Aborted);
    CHECK(r.phase == TransferPhase::Stream);
    CHECK(r.target_erased);
    CHECK(r.bytes_sent == 244 * 37);
    CHECK(bl.received.size() == 244 * 37);
    CHECK(bl.connected);  // the caller still holds the link and can send SYS_RESET
}

TEST_CASE("Abort after the last packet is ignored: the transfer completes", "[transfer][abort]") {
    const auto img = make_image(244 * 40 + 16);
    FakeBootloader bl;
    bl.cancel_after_bytes = static_cast<uint32_t>(img.size());
    VectorImage src(img);
    const auto r = dfu::run_transfer(bl, src, static_cast<uint32_t>(img.size()), make_dat(img), params(244));
    INFO(dfu::to_string(r.error) << " in " << dfu::to_string(r.phase));
    CHECK(r.ok());
    CHECK(bl.activated);
}
