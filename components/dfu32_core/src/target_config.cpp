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
    if (t.prn < 1 || t.prn > kMaxPrn) return "prn must be 1-8";
    if (t.interval_ms < kMinIntervalMs || t.interval_ms > kMaxIntervalMs) return "interval_ms must be 8-100";
    if (t.max_attempts < 1 || t.max_attempts > kMaxAttemptsLimit) return "max_attempts must be 1-10";
    if (t.arm_timeout_s < kMinArmTimeoutS || t.arm_timeout_s > kMaxArmTimeoutS) return "arm_timeout_s must be 10-3600";
    if (t.start_settle_ms > kMaxStartSettleMs) return "start_settle_ms must be 0-5000";
    if (t.prn_timeout_ms < kMinPrnTimeoutMs || t.prn_timeout_ms > kMaxPrnTimeoutMs) {
        return "prn_timeout_ms must be 500-60000";
    }
    if (t.last_ok_version.size() > kMaxVersionLen) return "last_ok_version longer than 63 characters";
    return {};
}

void write_json(json::ObjectWriter& w, const TargetConfig& t) {
    w.add("id", t.id)
        .add("name", t.name)
        .add("board", t.board)
        .add("mac", t.mac)
        .add("addr_type", int64_t{t.addr_type})
        .add("manifest_url", t.manifest_url)
        .add("max_app_size", int64_t{t.max_app_size})
        .add("prn", int64_t{t.prn})
        .add("interval_ms", int64_t{t.interval_ms})
        .add("max_attempts", int64_t{t.max_attempts})
        .add("arm_timeout_s", int64_t{t.arm_timeout_s})
        .add("start_settle_ms", int64_t{t.start_settle_ms})
        .add("prn_timeout_ms", int64_t{t.prn_timeout_ms})
        .add("last_ok_version", t.last_ok_version)
        .add("last_ok_at", static_cast<int64_t>(t.last_ok_at));
}

unsigned sanitize(TargetConfig& t) {
    const TargetConfig d;
    unsigned n = 0;
    const auto fix = [&](bool bad, auto& field, const auto& def) {
        if (bad) {
            field = def;
            ++n;
        }
    };
    fix(!validate_target_id(t.id).empty(), t.id, d.id);
    fix(t.name.size() > 63, t.name, d.name);
    fix(!validate_board(t.board).empty(), t.board, d.board);
    fix(!validate_mac(t.mac).empty(), t.mac, d.mac);
    fix(t.addr_type > 1, t.addr_type, d.addr_type);
    fix(!validate_manifest_url(t.manifest_url).empty(), t.manifest_url, d.manifest_url);
    fix(!validate_max_app_size(t.max_app_size).empty(), t.max_app_size, d.max_app_size);
    fix(t.prn < 1 || t.prn > kMaxPrn, t.prn, d.prn);
    fix(t.interval_ms < kMinIntervalMs || t.interval_ms > kMaxIntervalMs, t.interval_ms, d.interval_ms);
    fix(t.max_attempts < 1 || t.max_attempts > kMaxAttemptsLimit, t.max_attempts, d.max_attempts);
    fix(t.arm_timeout_s < kMinArmTimeoutS || t.arm_timeout_s > kMaxArmTimeoutS, t.arm_timeout_s, d.arm_timeout_s);
    fix(t.start_settle_ms > kMaxStartSettleMs, t.start_settle_ms, d.start_settle_ms);
    fix(t.prn_timeout_ms < kMinPrnTimeoutMs || t.prn_timeout_ms > kMaxPrnTimeoutMs, t.prn_timeout_ms,
        d.prn_timeout_ms);
    fix(t.last_ok_version.size() > kMaxVersionLen, t.last_ok_version, d.last_ok_version);
    return n;
}

std::string to_json(const TargetConfig& t) {
    json::ObjectWriter w;
    write_json(w, t);
    return w.str();
}

namespace {

enum class Field : uint8_t { Str, U8, U16, U32, U64 };

struct FieldDef {
    const char* key;
    Field type;
    bool writable;  // settable through apply_json()
    int64_t max;
};

constexpr FieldDef kFields[] = {
    {"id", Field::Str, true, 0},
    {"name", Field::Str, true, 0},
    {"board", Field::Str, true, 0},
    {"mac", Field::Str, true, 0},
    {"addr_type", Field::U8, true, 1},
    {"manifest_url", Field::Str, true, 0},
    {"max_app_size", Field::U32, true, UINT32_MAX},
    {"prn", Field::U16, true, UINT16_MAX},
    {"interval_ms", Field::U16, true, UINT16_MAX},
    {"max_attempts", Field::U16, true, UINT16_MAX},
    {"arm_timeout_s", Field::U16, true, UINT16_MAX},
    {"start_settle_ms", Field::U16, true, UINT16_MAX},
    {"prn_timeout_ms", Field::U16, true, UINT16_MAX},
    {"last_ok_version", Field::Str, false, 0},
    {"last_ok_at", Field::U64, false, INT64_MAX},
};

std::string* str_field(TargetConfig& t, std::string_view key) {
    if (key == "id") return &t.id;
    if (key == "name") return &t.name;
    if (key == "board") return &t.board;
    if (key == "mac") return &t.mac;
    if (key == "manifest_url") return &t.manifest_url;
    if (key == "last_ok_version") return &t.last_ok_version;
    return nullptr;
}

void set_int(TargetConfig& t, std::string_view key, int64_t v) {
    if (key == "addr_type") t.addr_type = static_cast<uint8_t>(v);
    else if (key == "max_app_size") t.max_app_size = static_cast<uint32_t>(v);
    else if (key == "prn") t.prn = static_cast<uint16_t>(v);
    else if (key == "interval_ms") t.interval_ms = static_cast<uint16_t>(v);
    else if (key == "max_attempts") t.max_attempts = static_cast<uint16_t>(v);
    else if (key == "arm_timeout_s") t.arm_timeout_s = static_cast<uint16_t>(v);
    else if (key == "start_settle_ms") t.start_settle_ms = static_cast<uint16_t>(v);
    else if (key == "prn_timeout_ms") t.prn_timeout_ms = static_cast<uint16_t>(v);
    else if (key == "last_ok_at") t.last_ok_at = static_cast<uint64_t>(v);
}

// Reads one field from `f` into `t`; "" or the reason it is wrongly typed.
std::string read_field(const FieldDef& d, const json::Node& f, TargetConfig& t) {
    if (d.type == Field::Str) {
        if (!f.is_string()) return std::string(d.key) + " must be a string";
        *str_field(t, d.key) = f.str();
        return {};
    }
    int64_t i = 0;
    if (!f.integer(i) || i < 0 || i > d.max) return std::string(d.key) + " must be an integer 0-" + std::to_string(d.max);
    set_int(t, d.key, i);
    return {};
}

}  // namespace

bool from_json(std::string_view text, TargetConfig& out) {
    json::Doc doc;
    if (!doc.parse(text)) return false;
    const json::Node v = doc.root();
    if (!v.is_object()) return false;
    TargetConfig t;
    for (const FieldDef& d : kFields) {
        const json::Node f = v.get(d.key);
        if (f.missing()) continue;
        if (!read_field(d, f, t).empty()) return false;
    }
    out = std::move(t);
    return true;
}

std::string apply_json(std::string_view body, const std::string& id, TargetConfig& t) {
    json::Doc doc;
    std::string err;
    if (!doc.parse(body, &err)) return err;
    const json::Node v = doc.root();
    if (!v.is_object()) return "body must be a JSON object";
    // Refuse unknown keys first, so a typo doesn't silently do nothing.
    for (const json::Node& item : v.items()) {
        bool known = false;
        for (const FieldDef& d : kFields) known = known || item.key() == d.key;
        if (!known) return "unknown field '" + std::string(item.key()) + "'";
    }
    // Start from a clean copy: a stored field that breaks a newer rule falls
    // back to its default instead of making every PUT fail.
    TargetConfig next = t;
    sanitize(next);
    for (const FieldDef& d : kFields) {
        const json::Node f = v.get(d.key);
        if (f.missing() || !d.writable) continue;
        if (auto e = read_field(d, f, next); !e.empty()) return e;
    }
    if (!v.get("id").missing() && next.id != id) return "body id does not match the URL";
    next.id = id;
    if (!next.mac.empty()) {
        const std::string m = normalize_mac(next.mac);
        if (!m.empty()) next.mac = m;
    }
    if (auto e = validate(next); !e.empty()) return e;
    t = std::move(next);
    return {};
}

}  // namespace dfu32
