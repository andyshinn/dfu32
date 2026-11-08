// SPDX-License-Identifier: MIT
//
// Improv Serial framing on a port shared with the console (SPEC.md §9.2).
// Pure C++, host-tested; the firmware's console reader feeds it every byte.
//
// A frame is  "IMPROV" | version (1) | type | length | data[length] | checksum
// where the checksum is the low byte of the sum of everything before it. The
// browser client follows each frame with a newline, which is swallowed here.
//
// Resync rules:
//  - bytes that match the start of "IMPROV" are held; at the first mismatch
//    (or kHeaderHoldMs of silence) they go back to the console unchanged;
//  - once the header is complete the bytes belong to the frame. A bad
//    checksum, a length over kMaxData, or kFrameTimeoutMs without a byte drops
//    it and the reader is back in console mode. A frame too long to keep is
//    still read to its declared end, so its data can't reach the console.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "improv/core.hpp"

namespace dfu32::improv {

// Serial message types (identical to improv::ImprovSerialType in the SDK).
inline constexpr uint8_t kSerialCurrentState = 0x01;  // device -> client
inline constexpr uint8_t kSerialErrorState = 0x02;    // device -> client
inline constexpr uint8_t kSerialRpc = 0x03;           // client -> device
inline constexpr uint8_t kSerialRpcResult = 0x04;     // device -> client
inline constexpr uint8_t kSerialVersion = 1;

class SerialFramer {
public:
    /// Held header bytes go back to the console after this much silence (a
    /// client sends a frame in one write, a person types slower).
    static constexpr uint32_t kHeaderHoldMs = 100;
    /// A frame that stops arriving for this long is dropped.
    static constexpr uint32_t kFrameTimeoutMs = 250;
    /// Largest data field kept (an RPC packet; WiFi settings are at most 100).
    static constexpr size_t kMaxData = kMaxPacket;

    enum class Event : uint8_t {
        None,
        Frame,        // type() / data() hold a frame with a good checksum
        BadChecksum,  // dropped
        BadVersion,   // dropped (checksum was fine)
        TooLong,      // dropped once its declared length had been read
        TimedOut,     // dropped: the rest never came
    };

    /// One byte received at `now_ms`. Bytes for the console (including held
    /// header bytes handed back) are appended to `console`. The frame from a
    /// Frame event stays readable until the next push() or poll().
    Event push(uint8_t byte, uint32_t now_ms, std::vector<uint8_t>& console);

    /// Call when no byte arrived: applies the hold and frame timeouts.
    Event poll(uint32_t now_ms, std::vector<uint8_t>& console);

    /// Milliseconds the reader may wait for input before poll() has work to
    /// do; kNoDeadline while nothing is pending.
    static constexpr uint32_t kNoDeadline = UINT32_MAX;
    uint32_t wait_ms(uint32_t now_ms) const;

    uint8_t type() const noexcept { return type_; }
    std::span<const uint8_t> data() const noexcept { return {buf_.data() + kHeaderLen, len_}; }

    /// Back to console mode; wipes the buffer (WiFi settings carry a passphrase).
    void reset() noexcept;
    bool idle() const noexcept { return phase_ == Phase::Console && !swallow_lf_; }

    static constexpr size_t kMagicLen = 6;       // "IMPROV"
    static constexpr size_t kHeaderLen = 9;      // magic, version, type, length

private:
    enum class Phase : uint8_t { Console, Magic, Fields, Data, Checksum, Skip };

    Event expire(uint32_t now_ms, std::vector<uint8_t>& console);
    Event finish(Event e);
    void hand_back(std::vector<uint8_t>& console);
    void clear_frame() noexcept;

    Phase phase_ = Phase::Console;
    size_t pos_ = 0;           // bytes of the frame received so far (Magic..Checksum)
    size_t skip_left_ = 0;     // Skip: declared bytes still to discard
    uint8_t type_ = 0;
    uint8_t len_ = 0;
    bool swallow_lf_ = false;  // the client's newline right after a frame
    bool frame_ready_ = false; // buf_ holds the last Frame until the next call
    uint32_t last_ms_ = 0;     // last byte that belonged to a frame (or the hold)
    std::array<uint8_t, kHeaderLen + kMaxData> buf_{};
};

/// A complete frame for the client: magic, version, type, length, data and
/// checksum (no surrounding newlines; the transport adds those).
std::vector<uint8_t> encode_serial_frame(uint8_t type, std::span<const uint8_t> data);

}  // namespace dfu32::improv
