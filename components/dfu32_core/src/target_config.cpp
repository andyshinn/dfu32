// SPDX-License-Identifier: MIT
#include "dfu32_core/target_config.hpp"

#include <cctype>

#include "dfu32_core/manifest.hpp"
#include "dfu32_core/url.hpp"

namespace dfu32 {

std::string validate_target_id(const std::string& id) {
    if (id.size() > 31) return "id longer than 31 characters";
    for (const char c : id) {
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-')) {
            return "id may contain only a-z, 0-9 and '-'";
        }
    }
    return {};
}

std::string validate_board(const std::string& board) {
    if (board.size() > kMaxBoardLen) return "board longer than 31 characters";
    for (const char c : board) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) return "board may not contain spaces or control characters";
    }
    return {};
}

std::string normalize_mac(const std::string& mac) {
    if (mac.size() != 17) return {};
    std::string out = mac;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i % 3 == 2) {
            if (out[i] != ':') return {};
        } else {
            if (!std::isxdigit(static_cast<unsigned char>(out[i]))) return {};
            out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
        }
    }
    return out;
}

std::string validate_mac(const std::string& mac) {
    if (mac.empty()) return {};
    return normalize_mac(mac).empty() ? "MAC must look like AA:BB:CC:DD:EE:FF" : std::string{};
}

std::string validate_manifest_url(const std::string& url) {
    if (url.empty()) return {};
    if (url.size() > kMaxUrlLen) return "manifest URL longer than 255 characters";
    Url u;
    std::string why;
    if (!parse_http_url(url, u, &why)) return "manifest URL: " + why;
    return {};
}

std::string validate(const TargetConfig& t) {
    if (auto e = validate_target_id(t.id); !e.empty()) return e;
    if (t.name.size() > 63) return "name longer than 63 characters";
    if (auto e = validate_board(t.board); !e.empty()) return e;
    if (auto e = validate_mac(t.mac); !e.empty()) return e;
    if (t.addr_type > 1) return "addr_type must be 0 (public) or 1 (random)";
    if (auto e = validate_manifest_url(t.manifest_url); !e.empty()) return e;
    return {};
}

}  // namespace dfu32
