// SPDX-License-Identifier: MIT
#include "improv/serial_frame.hpp"

namespace dfu32::improv {
namespace {

constexpr char kMagic[] = "IMPROV";

uint32_t elapsed(uint32_t now_ms, uint32_t since_ms) { return now_ms - since_ms; }  // wraps correctly

}  // namespace

void SerialFramer::clear_frame() noexcept {
    wipe(std::span<uint8_t>(buf_.data(), buf_.size()));
    pos_ = 0;
    skip_left_ = 0;
    type_ = 0;
    len_ = 0;
    frame_ready_ = false;
}

void SerialFramer::reset() noexcept {
    clear_frame();
    phase_ = Phase::Console;
    swallow_lf_ = false;
}

void SerialFramer::hand_back(std::vector<uint8_t>& console) {
    console.insert(console.end(), buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
    clear_frame();
    phase_ = Phase::Console;
}

SerialFramer::Event SerialFramer::finish(Event e) {
    clear_frame();
    phase_ = Phase::Console;
    // A frame that was read to its end is followed by the client's newline; a
    // cut-off one isn't (and the next byte may be someone typing).
    swallow_lf_ = e != Event::TimedOut;
    return e;
}

SerialFramer::Event SerialFramer::expire(uint32_t now_ms, std::vector<uint8_t>& console) {
    if (frame_ready_) clear_frame();
    const uint32_t quiet = elapsed(now_ms, last_ms_);
    if (swallow_lf_ && quiet >= kFrameTimeoutMs) swallow_lf_ = false;
    switch (phase_) {
        case Phase::Console: return Event::None;
        case Phase::Magic:
            if (quiet >= kHeaderHoldMs) hand_back(console);
            return Event::None;
        case Phase::Fields:
        case Phase::Data:
        case Phase::Checksum:
        case Phase::Skip:
            return quiet >= kFrameTimeoutMs ? finish(Event::TimedOut) : Event::None;
    }
    return Event::None;
}

SerialFramer::Event SerialFramer::poll(uint32_t now_ms, std::vector<uint8_t>& console) {
    return expire(now_ms, console);
}

SerialFramer::Event SerialFramer::push(uint8_t byte, uint32_t now_ms, std::vector<uint8_t>& console) {
    // A late byte first ends whatever timed out before it arrived.
    const Event expired = expire(now_ms, console);
    const auto result = [expired](Event e) { return e == Event::None ? expired : e; };

    if (swallow_lf_) {
        swallow_lf_ = false;
        if (byte == '\n') return result(Event::None);
    }

    switch (phase_) {
        case Phase::Console:
            if (byte == static_cast<uint8_t>(kMagic[0])) {
                buf_[0] = byte;
                pos_ = 1;
                phase_ = Phase::Magic;
                last_ms_ = now_ms;
            } else {
                console.push_back(byte);
            }
            return result(Event::None);

        case Phase::Magic:
            if (byte != static_cast<uint8_t>(kMagic[pos_])) {
                // Not a frame: the held bytes were console input. "IMPROV" has
                // no repeated prefix, so the new byte can only start a new one.
                hand_back(console);
                return result(push(byte, now_ms, console));
            }
            buf_[pos_++] = byte;
            last_ms_ = now_ms;
            if (pos_ == kMagicLen) phase_ = Phase::Fields;
            return result(Event::None);

        case Phase::Fields:
            buf_[pos_++] = byte;
            last_ms_ = now_ms;
            if (pos_ == kHeaderLen) {
                type_ = buf_[7];
                len_ = buf_[8];
                if (len_ > kMaxData) {
                    skip_left_ = size_t{len_} + 1;  // data and checksum
                    phase_ = Phase::Skip;
                } else {
                    phase_ = len_ == 0 ? Phase::Checksum : Phase::Data;
                }
            }
            return result(Event::None);

        case Phase::Data:
            buf_[pos_++] = byte;
            last_ms_ = now_ms;
            if (pos_ == kHeaderLen + len_) phase_ = Phase::Checksum;
            return result(Event::None);

        case Phase::Checksum: {
            last_ms_ = now_ms;
            uint8_t sum = 0;
            for (size_t i = 0; i < pos_; ++i) sum = static_cast<uint8_t>(sum + buf_[i]);
            if (sum != byte) return result(finish(Event::BadChecksum));
            if (buf_[kMagicLen] != kSerialVersion) return result(finish(Event::BadVersion));
            phase_ = Phase::Console;
            swallow_lf_ = true;
            frame_ready_ = true;  // data() is valid until the next call
            return Event::Frame;
        }

        case Phase::Skip:
            last_ms_ = now_ms;
            if (--skip_left_ == 0) return result(finish(Event::TooLong));
            return result(Event::None);
    }
    return result(Event::None);
}

uint32_t SerialFramer::wait_ms(uint32_t now_ms) const {
    uint32_t limit = 0;
    switch (phase_) {
        case Phase::Console:
            if (!swallow_lf_) return kNoDeadline;
            limit = kFrameTimeoutMs;
            break;
        case Phase::Magic: limit = kHeaderHoldMs; break;
        default: limit = kFrameTimeoutMs; break;
    }
    const uint32_t quiet = elapsed(now_ms, last_ms_);
    return quiet >= limit ? 0 : limit - quiet;
}

std::vector<uint8_t> encode_serial_frame(uint8_t type, std::span<const uint8_t> data) {
    if (data.size() > 255) return {};
    std::vector<uint8_t> out;
    out.reserve(SerialFramer::kHeaderLen + data.size() + 1);
    out.insert(out.end(), kMagic, kMagic + SerialFramer::kMagicLen);
    out.push_back(kSerialVersion);
    out.push_back(type);
    out.push_back(static_cast<uint8_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    uint8_t sum = 0;
    for (const uint8_t b : out) sum = static_cast<uint8_t>(sum + b);
    out.push_back(sum);
    return out;
}

}  // namespace dfu32::improv
