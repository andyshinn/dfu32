// SPDX-License-Identifier: MIT
#include "dfu32_core/api_core.hpp"

#include <cctype>
#include <initializer_list>

#include "dfu32_core/json.hpp"
#include "dfu32_core/manifest.hpp"
#include "dfu32_core/target_config.hpp"
#include "dfu32_core/target_table.hpp"

namespace dfu32::api {

// ---- routes ----------------------------------------------------------------------

namespace {

constexpr std::string_view kPrefix = "/api/v1/";

// Splits "a/b/c" into up to 4 segments; false if there are more or any is empty.
bool split(std::string_view path, std::string_view (&seg)[4], size_t& n) {
    n = 0;
    while (!path.empty()) {
        const size_t slash = path.find('/');
        const std::string_view s = path.substr(0, slash);
        if (s.empty() || n == 4) return false;
        seg[n++] = s;
        if (slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
    return true;
}

}  // namespace

RouteMatch match_route(std::string_view uri) {
    RouteMatch m;
    if (const size_t q = uri.find('?'); q != std::string_view::npos) uri = uri.substr(0, q);
    if (uri.substr(0, kPrefix.size()) != kPrefix) return m;
    uri.remove_prefix(kPrefix.size());
    if (!uri.empty() && uri.back() == '/') uri.remove_suffix(1);
    std::string_view s[4];
    size_t n = 0;
    if (uri.empty() || !split(uri, s, n)) return m;

    const auto is = [&](size_t count, std::string_view a, std::string_view b = {}, std::string_view c = {}) {
        if (n != count || s[0] != a) return false;
        if (count >= 2 && !b.empty() && s[1] != b) return false;
        if (count >= 3 && !c.empty() && s[2] != c) return false;
        return true;
    };

    if (is(1, "status")) m.route = Route::Status;
    else if (is(1, "claim")) m.route = Route::Claim;
    else if (is(1, "targets")) m.route = Route::Targets;
    else if (is(1, "staging")) m.route = Route::Staging;
    else if (is(1, "logs")) m.route = Route::Logs;
    else if (is(1, "config")) m.route = Route::Config;
    else if (is(2, "ble", "scan")) m.route = Route::BleScan;
    else if (is(2, "system", "reboot")) m.route = Route::Reboot;
    else if (is(2, "system", "ota")) m.route = Route::SystemOta;
    else if (is(3, "system", "token", "rotate")) m.route = Route::TokenRotate;
    else if (is(3, "system", "token", "clear")) m.route = Route::TokenClear;
    else if (is(3, "jobs", "current", "abort")) m.route = Route::JobAbort;
    else if (is(2, "debug", "fault")) m.route = Route::DebugFault;
    else if (is(2, "runs")) {
        uint32_t seq = 0;
        if (parse_u32(s[1], seq) && seq != 0 && s[1][0] != '0') {
            m.route = Route::Run;
            m.seq = seq;
        }
    } else if (n >= 2 && s[0] == "targets") {
        m.id = std::string(s[1]);
        if (n == 2) m.route = Route::Target;
        else if (n == 3 && s[2] == "check") m.route = Route::TargetCheck;
        else if (n == 3 && s[2] == "stage") m.route = Route::TargetStage;
        else if (n == 3 && s[2] == "flash") m.route = Route::TargetFlash;
        else if (n == 3 && s[2] == "runs") m.route = Route::TargetRuns;
        else if (n == 3 && s[2] == "default") m.route = Route::TargetDefault;
        if (m.route == Route::NotFound) m.id.clear();
    }
    return m;
}

bool method_allowed(Route r, Method m) noexcept {
    switch (r) {
        case Route::NotFound: return false;
        case Route::Status:
        case Route::Targets:
        case Route::TargetRuns:
        case Route::Run:
        case Route::Logs: return m == Method::Get;
        case Route::Target: return m == Method::Get || m == Method::Put || m == Method::Delete;
        case Route::Staging: return m == Method::Get || m == Method::Post || m == Method::Delete;
        case Route::Config: return m == Method::Get || m == Method::Put;
        case Route::Claim:
        case Route::TargetCheck:
        case Route::TargetStage:
        case Route::TargetFlash:
        case Route::TargetDefault:
        case Route::JobAbort:
        case Route::BleScan:
        case Route::Reboot:
        case Route::TokenRotate:
        case Route::TokenClear:
        case Route::SystemOta:
        case Route::DebugFault: return m == Method::Post;
    }
    return false;
}

bool is_public(Route r) noexcept { return r == Route::Status || r == Route::Claim; }

std::string_view query_param(std::string_view uri, std::string_view key) {
    const size_t q = uri.find('?');
    if (q == std::string_view::npos) return {};
    std::string_view rest = uri.substr(q + 1);
    while (!rest.empty()) {
        const size_t amp = rest.find('&');
        const std::string_view pair = rest.substr(0, amp);
        const size_t eq = pair.find('=');
        if (pair.substr(0, eq) == key) return eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1);
        if (amp == std::string_view::npos) break;
        rest.remove_prefix(amp + 1);
    }
    return {};
}

namespace {

int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool has_control(std::string_view s) noexcept {
    for (const char c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return true;
    }
    return false;
}

/// At most `max` bytes, never splitting a UTF-8 sequence.
std::string clip_utf8(std::string s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return s;
}

}  // namespace

bool query_text(std::string_view uri, std::string_view key, std::string& out) {
    out.clear();
    const std::string_view raw = query_param(uri, key);
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '+') {
            out += ' ';
        } else if (raw[i] == '%') {
            if (i + 2 >= raw.size()) return false;  // needs two hex digits after it
            const int hi = hex_digit(raw[i + 1]), lo = hex_digit(raw[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out += static_cast<char>(hi << 4 | lo);
            i += 2;
        } else {
            out += raw[i];
        }
    }
    return true;
}

std::string parse_staging_upload(std::string_view uri, StagingUpload& out) {
    out = StagingUpload{};
    std::string text;
    if (!query_text(uri, "dat", text)) return "dat: malformed %-escape";
    if (text.empty()) return "dat is required: the init packet (.dat) as hex";
    if (text.size() % 2 != 0 || text.size() > 2 * 64) return "dat must be 1-64 bytes as hex";
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = hex_digit(text[i]), lo = hex_digit(text[i + 1]);
        if (hi < 0 || lo < 0) return "dat must be hex";
        out.dat.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }

    if (!query_text(uri, "version", out.version)) return "version: malformed %-escape";
    if (out.version.empty() || out.version.size() > kMaxVersionLen) {
        return "version is required: 1-" + std::to_string(kMaxVersionLen) + " bytes, as the repeater's ver prints it";
    }
    if (has_control(out.version)) return "version has control characters";

    if (!query_text(uri, "board", out.board)) return "board: malformed %-escape";
    if (auto e = validate_board(out.board); !e.empty()) return e;

    if (!query_text(uri, "sha256", text)) return "sha256: malformed %-escape";
    if (!text.empty()) {
        if (!parse_sha256_hex(text, out.sha256)) return "sha256 must be 64 hex digits";
        out.has_sha256 = true;
    }

    std::string name;
    if (!query_text(uri, "name", name)) return "name: malformed %-escape";
    if (has_control(name)) return "name has control characters";
    out.source = clip_utf8(name.empty() ? std::string("upload") : "upload:" + name, kMaxUrlLen);

    if (!query_text(uri, "target", out.target)) return "target: malformed %-escape";
    if (auto e = validate_target_id(out.target); !e.empty()) return "target: " + e;
    return {};
}

const StoredTarget* staging_target_for(const TargetTable& table, std::string_view image_board) {
    if (const StoredTarget* d = table.default_target(); d != nullptr && staged_board_ok(image_board, d->cfg.board)) {
        return d;
    }
    for (const StoredTarget& st : table.targets()) {
        if (staged_board_ok(image_board, st.cfg.board)) return &st;
    }
    return nullptr;
}

// ---- auth --------------------------------------------------------------------------

std::string base64url_encode(std::span<const uint8_t> data) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((data.size() * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const uint32_t v = uint32_t{data[i]} << 16 | uint32_t{data[i + 1]} << 8 | data[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    const size_t rest = data.size() - i;
    if (rest == 1) {
        const uint32_t v = uint32_t{data[i]} << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
    } else if (rest == 2) {
        const uint32_t v = uint32_t{data[i]} << 16 | uint32_t{data[i + 1]} << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
    }
    return out;
}

bool ct_equal(std::span<const uint8_t> a, std::span<const uint8_t> b) noexcept {
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

std::string_view bearer_token(std::string_view header) noexcept {
    constexpr std::string_view kScheme = "bearer ";
    if (header.size() <= kScheme.size()) return {};
    for (size_t i = 0; i < kScheme.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(header[i])) != kScheme[i]) return {};
    }
    header.remove_prefix(kScheme.size());
    while (!header.empty() && header.front() == ' ') header.remove_prefix(1);
    while (!header.empty() && (header.back() == ' ' || header.back() == '\t')) header.remove_suffix(1);
    return header;
}

void AuthThrottle::record_failure(uint64_t now_ms) noexcept {
    if (failures_ == 0 || now_ms - window_start_ > kWindowMs) {
        failures_ = 0;
        window_start_ = now_ms;
    }
    ++failures_;
    if (failures_ >= kMaxFailures) penalty_until_ = now_ms + kPenaltyMs;
}

bool AuthThrottle::try_attempt(uint64_t now_ms) noexcept {
    if (!penalised(now_ms)) return true;
    if (now_ms < next_slot_) return false;
    next_slot_ = now_ms + kSlotMs;
    return true;
}

// ---- device settings --------------------------------------------------------------

std::string validate_ntp_server(const std::string& s) {
    if (s.empty()) return {};
    if (s.size() > 63) return "ntp_server longer than 63 characters";
    char prev = '.';
    for (const char c : s) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.';
        if (!ok) return "ntp_server may contain only letters, digits, '-' and '.'";
        if (c == '.' && (prev == '.' || prev == '-')) return "ntp_server is not a valid host name";
        if (c == '-' && prev == '.') return "ntp_server is not a valid host name";
        prev = c;
    }
    if (prev == '.' || prev == '-') return "ntp_server is not a valid host name";
    return {};
}

std::string validate(const DeviceConfig& c) {
    if (!c.hostname.empty()) {
        if (auto e = validate_hostname(c.hostname); !e.empty()) return e;
    }
    if (auto e = validate_ntp_server(c.ntp_server); !e.empty()) return e;
    if (c.dfu_deadline_max_s < kMinDeadlineMaxS || c.dfu_deadline_max_s > kMaxDeadlineMaxS) {
        return "dfu_deadline_max_s must be 120-7200";
    }
    return {};
}

void write_json(json::ObjectWriter& w, const DeviceConfig& c) {
    w.add("hostname", c.hostname)
        .add("ntp_server", c.ntp_server)
        .add_bool("wifi_off_dfu", c.wifi_off_dfu)
        .add("dfu_deadline_max_s", int64_t{c.dfu_deadline_max_s});
}

std::string apply_json(std::string_view body, DeviceConfig& c) {
    json::Doc doc;
    std::string err;
    if (!doc.parse(body, &err)) return err;
    const json::Node v = doc.root();
    if (!v.is_object()) return "body must be a JSON object";
    DeviceConfig next = c;
    for (const json::Node& f : v.items()) {
        const std::string_view k = f.key();
        if (k == "hostname" || k == "ntp_server") {
            if (!f.is_string()) return std::string(k) + " must be a string";
            (k == "hostname" ? next.hostname : next.ntp_server) = f.str();
        } else if (k == "wifi_off_dfu") {
            if (!f.is_bool()) return "wifi_off_dfu must be true or false";
            next.wifi_off_dfu = f.boolean();
        } else if (k == "dfu_deadline_max_s") {
            int64_t i = 0;
            if (!f.integer(i) || i < 0 || i > UINT32_MAX) return "dfu_deadline_max_s must be an integer";
            next.dfu_deadline_max_s = static_cast<uint32_t>(i);
        } else {
            return "unknown field '" + std::string(k) + "'";
        }
    }
    if (auto e = validate(next); !e.empty()) return e;
    c = std::move(next);
    return {};
}

// ---- BLE scan view ----------------------------------------------------------------

namespace {

bool blank(std::string_view body) {
    for (const char c : body) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return true;
}

// Reads an integer field in [lo, hi] if present; "" or the reason.
std::string read_u32(const json::Node& root, const char* key, uint32_t lo, uint32_t hi, uint32_t& out) {
    const json::Node f = root.get(key);
    if (f.missing()) return {};
    int64_t v = 0;
    if (!f.integer(v) || v < lo || v > hi) {
        return std::string(key) + " must be " + std::to_string(lo) + "-" + std::to_string(hi);
    }
    out = static_cast<uint32_t>(v);
    return {};
}

std::string read_bool(const json::Node& root, const char* key, bool& out) {
    const json::Node f = root.get(key);
    if (f.missing()) return {};
    if (!f.is_bool()) return std::string(key) + " must be true or false";
    out = f.boolean();
    return {};
}

std::string object_body(std::string_view body, json::Doc& doc, std::initializer_list<std::string_view> known) {
    std::string err;
    if (!doc.parse(body, &err)) return err;
    if (!doc.root().is_object()) return "body must be a JSON object";
    for (const json::Node& f : doc.root().items()) {
        bool ok = false;
        for (const auto k : known) ok = ok || f.key() == k;
        if (!ok) return "unknown field '" + std::string(f.key()) + "'";
    }
    return {};
}

}  // namespace

std::string parse_scan_request(std::string_view body, ScanRequest& out) {
    ScanRequest r;
    if (!blank(body)) {
        json::Doc doc;
        if (auto e = object_body(body, doc, {"duration_s", "dfu_only"}); !e.empty()) return e;
        if (auto e = read_u32(doc.root(), "duration_s", 1, 30, r.duration_s); !e.empty()) return e;
        if (auto e = read_bool(doc.root(), "dfu_only", r.dfu_only); !e.empty()) return e;
    }
    out = r;
    return {};
}

std::string parse_fault_request(std::string_view body, FaultRequest& out) {
    FaultRequest r;
    if (!blank(body)) {
        json::Doc doc;
        if (auto e = object_body(body, doc, {"restore_fail", "deadline_s", "stall_s"}); !e.empty()) return e;
        if (auto e = read_bool(doc.root(), "restore_fail", r.restore_fail); !e.empty()) return e;
        if (auto e = read_u32(doc.root(), "deadline_s", 0, 3600, r.deadline_s); !e.empty()) return e;
        if (auto e = read_u32(doc.root(), "stall_s", 0, 600, r.stall_s); !e.empty()) return e;
    }
    out = r;
    return {};
}

namespace {

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool eq = true;
        for (size_t j = 0; j < needle.size() && eq; ++j) {
            eq = std::toupper(static_cast<unsigned char>(hay[i + j])) == needle[j];
        }
        if (eq) return true;
    }
    return false;
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && contains_ci(s.substr(s.size() - suffix.size()), suffix);
}

}  // namespace

const char* classify_dfu(std::string_view name, bool dfu_service) noexcept {
    if (contains_ci(name, "DFU")) return "bootloader";
    if (ends_with_ci(name, "_OTA")) return "app_buttonless";
    if (dfu_service) return "bootloader";
    return nullptr;
}

std::string mac_plus_one(const std::string& mac) {
    std::string m = normalize_mac(mac);
    if (m.empty()) return {};
    uint32_t last = 0;
    parse_u32("0x" + m.substr(15), last);
    static constexpr char kHex[] = "0123456789ABCDEF";
    last = (last + 1) & 0xFF;
    m[15] = kHex[last >> 4];
    m[16] = kHex[last & 15];
    return m;
}

bool matches_target(const std::string& addr, const std::string& target_mac) {
    const std::string a = normalize_mac(addr);
    const std::string t = normalize_mac(target_mac);
    if (a.empty() || t.empty()) return false;
    return a == t || a == mac_plus_one(t);
}

std::string matching_target_id(const TargetTable& table, const std::string& addr) {
    for (const StoredTarget& st : table.targets()) {
        if (matches_target(addr, st.cfg.mac)) return st.id();
    }
    return {};
}

}  // namespace dfu32::api
