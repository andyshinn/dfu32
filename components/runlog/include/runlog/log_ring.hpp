// SPDX-License-Identifier: MIT
//
// Byte ring for captured log output (SPEC.md §13.1). Pure and not thread-safe;
// the ESP-IDF glue (runlog_idf.hpp) serialises access and feeds it from the
// esp_log vprintf hook. Positions are absolute byte counts since start, so a
// caller can remember where a run began and later read everything since.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace runlog {

class LogRing {
public:
    explicit LogRing(std::span<char> storage) : buf_(storage) {}

    void append(std::string_view text) noexcept;

    /// Total bytes ever appended (monotonic).
    uint64_t position() const noexcept { return written_; }

    /// Oldest absolute position still held.
    uint64_t oldest() const noexcept { return written_ > buf_.size() ? written_ - buf_.size() : 0; }

    /// The last `max_bytes` of everything appended since `from` (clamped to what
    /// the ring still holds). If the result had to be cut mid-line, it starts
    /// after the first newline so it never begins with a partial line.
    std::string since(uint64_t from, size_t max_bytes) const;

    std::string tail(size_t max_bytes) const { return since(0, max_bytes); }

private:
    std::span<char> buf_;
    uint64_t written_ = 0;
};

}  // namespace runlog
