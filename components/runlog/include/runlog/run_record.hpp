// SPDX-License-Identifier: MIT
//
// JSON summary stored in each run slot (SPEC.md §13.2). Plain fields so the
// console now, and the HTTP API in M3, can render the same record.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace runlog {

struct RunAttempt {
    uint8_t number = 0;
    uint16_t interval_ms = 0;
    uint16_t prn = 0;
    std::string outcome;  // "OK" or the error text
    std::string kind;     // dfu::FailureKind name, "NONE" on success
    uint32_t bytes_acked = 0;
};

struct RunRecord {
    uint32_t seq = 0;
    std::string result;         // SUCCEEDED | FAILED | ABORTED_BY_REBOOT
    std::string error;          // empty on success
    std::string failure_kind;   // dfu::FailureKind name
    std::string target;         // app MAC
    std::string image_version;
    std::string image_sha256;   // hex
    uint32_t image_size = 0;
    uint32_t started_uptime_s = 0;
    uint32_t duration_ms = 0;
    std::string last_phase;
    uint32_t bytes_acked = 0;
    std::string target_erased;  // "yes" | "no" | "possibly"
    bool sys_reset_after_failure = false;
    bool wifi_off = false;            // WiFi was stopped for the transfer (SPEC.md §7.1)
    std::string bootloader;     // DIS firmware revision
    std::string bootloader_class;
    uint16_t mtu = 0;
    uint16_t packet_size = 0;
    uint16_t conn_interval_x125 = 0;  // 1.25 ms units, as negotiated
    uint32_t rate_x10_kBps = 0;       // stream throughput in 0.1 kB/s
    std::string reset_reason;         // ABORTED_BY_REBOOT only
    std::vector<RunAttempt> attempts;
};

std::string json_escape(std::string_view s);
std::string to_json(const RunRecord& r);

/// Minimal readers for the JSON this module writes (no general parser).
std::string json_get_string(std::string_view json, std::string_view key);
uint64_t json_get_uint(std::string_view json, std::string_view key, uint64_t fallback = 0);

}  // namespace runlog
