// SPDX-License-Identifier: MIT
#include "dfu32_core/target_config.hpp"

#include <cctype>

#include "dfu32_core/json.hpp"
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

std::string validate_max_app_size(uint32_t bytes) {
    if (bytes == 0) return {};
    if (bytes < 4096 || bytes > 16u * 1024 * 1024 || bytes % 4 != 0) {
        return "max app size must be 0 (default) or a multiple of 4 between 4096 and 16777216";
    }
    return {};
}

std::string validate_hostname(const std::string& name) {
    if (name.empty() || name.size() > 31) return "hostname must be 1-31 characters";
    for (const char c : name) {
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-')) {
            return "hostname may contain only a-z, 0-9 and '-'";
        }
    }
    if (name.front() == '-' || name.back() == '-') return "hostname may not start or end with '-'";
    return {};
}

bool parse_u32(std::string_view text, uint32_t& out) noexcept {
    unsigned base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) return false;
    uint64_t v = 0;
    for (const char c : text) {
        unsigned d;
        if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
        else return false;
        v = v * base + d;
        if (v > UINT32_MAX) return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
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
    if (auto e = validate_max_app_size(t.max_app_size); !e.empty()) return e;
    return {};
}

std::string to_json(const TargetConfig& t) {
    json::ObjectWriter w;
    w.add("id", t.id)
        .add("name", t.name)
        .add("board", t.board)
        .add("mac", t.mac)
        .add("addr_type", int64_t{t.addr_type})
        .add("manifest_url", t.manifest_url)
        .add("max_app_size", int64_t{t.max_app_size});
    return w.str();
}

bool from_json(std::string_view text, TargetConfig& out) {
    json::Doc doc;
    if (!doc.parse(text)) return false;
    const json::Node v = doc.root();
    if (!v.is_object()) return false;
    TargetConfig t;
    const auto str = [&](const char* key, std::string& dst) {
        const json::Node f = v.get(key);
        if (f.missing()) return true;
        if (!f.is_string()) return false;
        dst = f.str();
        return true;
    };
    if (!str("id", t.id) || !str("name", t.name) || !str("board", t.board) || !str("mac", t.mac) ||
        !str("manifest_url", t.manifest_url)) {
        return false;
    }
    int64_t i = 0;
    if (const json::Node f = v.get("addr_type"); !f.missing()) {
        if (!f.integer(i) || i < 0 || i > 1) return false;
        t.addr_type = static_cast<uint8_t>(i);
    }
    if (const json::Node f = v.get("max_app_size"); !f.missing()) {
        if (!f.integer(i) || i < 0 || i > UINT32_MAX) return false;
        t.max_app_size = static_cast<uint32_t>(i);
    }
    out = std::move(t);
    return true;
}

}  // namespace dfu32
