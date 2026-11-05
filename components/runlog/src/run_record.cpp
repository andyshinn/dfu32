// SPDX-License-Identifier: MIT
#include "runlog/run_record.hpp"

#include <cstdio>

namespace runlog {
namespace {

void str_field(std::string& o, const char* key, std::string_view v, bool comma = true) {
    o += '"';
    o += key;
    o += "\":\"";
    o += json_escape(v);
    o += '"';
    if (comma) o += ',';
}

void num_field(std::string& o, const char* key, uint64_t v, bool comma = true) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\"%s\":%llu", key, static_cast<unsigned long long>(v));
    o += buf;
    if (comma) o += ',';
}

void bool_field(std::string& o, const char* key, bool v, bool comma = true) {
    o += '"';
    o += key;
    o += "\":";
    o += v ? "true" : "false";
    if (comma) o += ',';
}

// Position just after `"key":` at the top level of a flat object we wrote.
size_t value_pos(std::string_view json, std::string_view key) {
    std::string needle = "\"";
    needle.append(key);
    needle += "\":";
    const size_t at = json.find(needle);
    return at == std::string_view::npos ? std::string_view::npos : at + needle.size();
}

}  // namespace

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string to_json(const RunRecord& r) {
    std::string o = "{";
    num_field(o, "seq", r.seq);
    str_field(o, "result", r.result);
    str_field(o, "error", r.error);
    str_field(o, "failure_kind", r.failure_kind);
    str_field(o, "target", r.target);
    str_field(o, "image_version", r.image_version);
    str_field(o, "image_sha256", r.image_sha256);
    num_field(o, "image_size", r.image_size);
    num_field(o, "started_uptime_s", r.started_uptime_s);
    num_field(o, "duration_ms", r.duration_ms);
    str_field(o, "last_phase", r.last_phase);
    num_field(o, "bytes_acked", r.bytes_acked);
    str_field(o, "target_erased", r.target_erased);
    bool_field(o, "sys_reset_after_failure", r.sys_reset_after_failure);
    bool_field(o, "wifi_off", r.wifi_off);
    str_field(o, "target_id", r.target_id);
    str_field(o, "trigger", r.trigger);
    str_field(o, "source", r.source);
    num_field(o, "started_unix", r.started_unix);
    str_field(o, "bootloader", r.bootloader);
    str_field(o, "bootloader_class", r.bootloader_class);
    num_field(o, "mtu", r.mtu);
    num_field(o, "packet_size", r.packet_size);
    num_field(o, "conn_interval_x125", r.conn_interval_x125);
    num_field(o, "rate_x10_kBps", r.rate_x10_kBps);
    str_field(o, "reset_reason", r.reset_reason);
    o += "\"attempts\":[";
    for (size_t i = 0; i < r.attempts.size(); ++i) {
        const RunAttempt& a = r.attempts[i];
        o += '{';
        num_field(o, "n", a.number);
        num_field(o, "interval_ms", a.interval_ms);
        num_field(o, "prn", a.prn);
        str_field(o, "outcome", a.outcome);
        str_field(o, "kind", a.kind);
        num_field(o, "bytes_acked", a.bytes_acked, false);
        o += '}';
        if (i + 1 < r.attempts.size()) o += ',';
    }
    o += "]}";
    return o;
}

std::string json_get_string(std::string_view json, std::string_view key) {
    size_t i = value_pos(json, key);
    if (i == std::string_view::npos || i >= json.size() || json[i] != '"') return {};
    std::string out;
    for (++i; i < json.size() && json[i] != '"'; ++i) {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            const char e = json[++i];
            switch (e) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u':
                    // Only \u00XX is ever written (control characters).
                    if (i + 4 < json.size()) {
                        unsigned v = 0;
                        for (size_t k = 1; k <= 4; ++k) {
                            const char h = json[i + k];
                            v = v * 16 + static_cast<unsigned>(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
                        }
                        c = static_cast<char>(v);
                        i += 4;
                    }
                    break;
                default: c = e; break;
            }
        }
        out += c;
    }
    return out;
}

uint64_t json_get_uint(std::string_view json, std::string_view key, uint64_t fallback) {
    size_t i = value_pos(json, key);
    if (i == std::string_view::npos || i >= json.size() || json[i] < '0' || json[i] > '9') return fallback;
    uint64_t v = 0;
    for (; i < json.size() && json[i] >= '0' && json[i] <= '9'; ++i) v = v * 10 + static_cast<uint64_t>(json[i] - '0');
    return v;
}

}  // namespace runlog
