// SPDX-License-Identifier: MIT
//
// The orchestrator (SPEC.md §7): one job at a time, run on its own task.
//  - stage: fetch + verify into the staging partition;
//  - flash: (fetch or reuse) -> verify -> precheck -> arm and wait for the
//    target -> radio handover -> DFU -> restore WiFi -> run record.
// It also owns the device-wide "BLE / staging busy" lease, so the console and
// the API's scan endpoint can't collide with a job (409 Busy).
#pragma once

#include <cstdint>
#include <string>

#include "dfu32_core/job.hpp"
#include "dfu32_core/target_config.hpp"
#include "dfu_ble/dfu_ble.hpp"
#include "runlog/run_record.hpp"
#include "staging/staging.hpp"

namespace dfu32::job {

/// Starts the job task. Call once, after BLE and net are up.
bool init();

// ---- busy lease ----------------------------------------------------------------------

/// Takes the device-wide lease for work outside a job (console flash/stage/
/// scan/probe, API scan). False if a job or another holder has it.
bool try_acquire(const char* who);
void release();
/// Current holder ("job", "console", "scan"), or nullptr when free.
const char* holder();

/// RAII form of try_acquire()/release().
class Lease {
public:
    explicit Lease(const char* who) : ok_(try_acquire(who)) {}
    ~Lease() {
        if (ok_) release();
    }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    bool ok() const { return ok_; }

private:
    bool ok_;
};

// ---- jobs ----------------------------------------------------------------------------

enum class SubmitResult : uint8_t { Ok, Busy, Invalid };

/// Queues a stage job for `target`. Invalid (with `why`) if the target has no
/// board or manifest URL.
SubmitResult submit_stage(const TargetConfig& target, const StageRequest& req, std::string& why,
                          uint32_t& job_id);

/// Queues a flash job (arm, then wait). Invalid if the target has no MAC, or
/// (source=manifest) no board / manifest URL.
SubmitResult submit_flash(const TargetConfig& target, const FlashRequest& req, std::string& why, uint32_t& job_id);

struct AbortResult {
    bool was_active = false;
    State state = State::Idle;   // state when the abort was requested
    bool target_erased = false;  // START was reached: the target's app is gone
};
/// Asks the running job to stop at its next check point.
AbortResult abort();

/// Current or last job (thread-safe copy).
Snapshot snapshot();

/// True while a job is running (not Idle and not finished).
bool busy();

// ---- run records (shared with the console's flash) -----------------------------------

struct RunContext {
    uint32_t seq = 0;
    std::string target_id;
    uint32_t started_uptime_s = 0;
    uint64_t started_unix = 0;
    bool wifi_off = false;
    const char* trigger = "";
    const char* source = "";
};

runlog::RunRecord make_run_record(const RunContext& ctx, const ble::Mac& mac, const StagedImage& img,
                                  const ble::FlashReport& r);

// ---- fault injection (CONFIG_DFU32_DEBUG_ENDPOINTS, SPEC.md §14.2) --------------------

struct Faults {
    bool restore_fail = false;  // the next restore "fails" twice -> restore watchdog reboots
    uint32_t deadline_s = 0;    // the next flash uses this run deadline instead
    uint32_t stall_s = 0;       // the next handover blocks this long without a heartbeat
};
/// Armed for the next flash job only.
void set_faults(const Faults& f);
Faults faults();

}  // namespace dfu32::job
