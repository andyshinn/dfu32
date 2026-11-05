// SPDX-License-Identifier: MIT
//
// ESP-IDF glue for runlog (SPEC.md §7.3, §13):
//  - log capture: an esp_log vprintf hook tees every log line into a LogRing
//    held in PSRAM, so runs can store their log tail and the console can show
//    recent output;
//  - the persisted RunLog on the raw `runlog` partition;
//  - the job marker in NVS (`job_active`): set while a flash runs, so a reboot
//    mid-run is recorded as ABORTED_BY_REBOOT on the next boot.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "runlog/run_log.hpp"

namespace runlog::idf {

// ---- log capture ----
/// Allocates the ring (PSRAM preferred) and installs the vprintf hook. Call
/// early in app_main; safe to call once.
bool log_capture_start(size_t bytes);
uint64_t log_position();
std::string log_since(uint64_t from, size_t max_bytes);
std::string log_tail(size_t max_bytes);
/// Like log_since(), and also returns the ring position the text ends at,
/// read under the same lock (the `next` cursor for GET /api/v1/logs).
std::string log_read(uint64_t from, size_t max_bytes, uint64_t& end);

// ---- run log ----
/// Opens the `runlog` partition and scans it. Returns nullptr if missing.
RunLog* open_run_log();

// ---- job marker ----
enum class JobStage : uint8_t {
    Connecting = 0,    // looking for / talking to the target; nothing erased yet
    Transferring = 1,  // START may have been sent: the target's app may be gone
};

struct JobMarker {
    uint32_t seq = 0;  // run record seq this job will be written as
    JobStage stage = JobStage::Connecting;
    uint32_t started_uptime_s = 0;
    char target[18] = {};
    char version[64] = {};
};

bool job_begin(const JobMarker& m);
bool job_set_stage(JobStage stage);
bool job_end();
/// True if a job marker survived a reboot (the previous run never finished).
bool job_pending(JobMarker& out);

/// Human-readable esp_reset_reason() for the last boot.
const char* reset_reason_string();

}  // namespace runlog::idf
