// SPDX-License-Identifier: MIT
#include "runlog/log_ring.hpp"

#include <algorithm>

namespace runlog {

void LogRing::append(std::string_view text) noexcept {
    const size_t cap = buf_.size();
    if (cap == 0 || text.empty()) return;
    if (text.size() > cap) {
        // Only the newest `cap` bytes can be kept.
        written_ += text.size() - cap;
        text.remove_prefix(text.size() - cap);
    }
    size_t at = static_cast<size_t>(written_ % cap);
    const size_t first = std::min(text.size(), cap - at);
    std::copy_n(text.data(), first, buf_.data() + at);
    std::copy_n(text.data() + first, text.size() - first, buf_.data());
    written_ += text.size();
}

std::string LogRing::since(uint64_t from, size_t max_bytes) const {
    const size_t cap = buf_.size();
    if (cap == 0 || max_bytes == 0) return {};
    uint64_t start = std::max(from, oldest());
    bool cut = start > from;
    if (written_ - start > max_bytes) {
        start = written_ - max_bytes;
        cut = true;
    }
    const size_t len = static_cast<size_t>(written_ - start);
    std::string out;
    out.reserve(len);
    const size_t at = static_cast<size_t>(start % cap);
    const size_t first = std::min(len, cap - at);
    out.append(buf_.data() + at, first);
    out.append(buf_.data(), len - first);
    // Trim a partial first line, unless `start` is known to be a line start:
    // the byte before it is still held and is a newline. At oldest() after a
    // wrap that byte is gone, so we can't know and trim.
    const bool at_line_start = start > oldest() && buf_[static_cast<size_t>((start - 1) % cap)] == '\n';
    if (cut && !at_line_start) {
        const size_t nl = out.find('\n');
        out.erase(0, nl == std::string::npos ? out.size() : nl + 1);
    }
    return out;
}

}  // namespace runlog
