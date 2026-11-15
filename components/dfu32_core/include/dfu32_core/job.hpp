// SPDX-License-Identifier: MIT
//
// Orchestrator job model (SPEC.md §7): states and legal transitions, request
// parsing for POST /stage and /flash, the run deadline (§7.3) and the job
// snapshot that GET /api/v1/status reports. Pure, so the state machine is
// host-tested; the task that drives it lives in components/job.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dfu32::json {
class ObjectWriter;
}

namespace dfu32::job {

enum class State : uint8_t {
    Idle,
    Fetching,
    VerifyingStaged,
    Precheck,
    Staged,          // stage-only job finished (terminal)
    AwaitingTarget,  // armed: scanning for the target, WiFi up
    RadioHandover,   // target seen: stopping WiFi (if configured)
    DfuRunning,
    Restoring,       // bringing WiFi back, writing the run record
    Succeeded,       // terminal
    Failed,          // terminal
};

enum class Kind : uint8_t { Stage, Flash };
enum class Source : uint8_t { Manifest, Staged };

const char* to_string(State s) noexcept;
const char* to_string(Kind k) noexcept;
const char* to_string(Source s) noexcept;

/// Staged, Succeeded, Failed.
bool is_terminal(State s) noexcept;
/// A job is running (not Idle and not finished).
bool is_active(State s) noexcept;
/// The edges of the SPEC.md §7 diagram, plus abort/error edges: any state
/// before the handover may fail directly; once the radio handover has begun,
/// the job always goes through Restoring so WiFi comes back.
bool can_transition(State from, State to, Kind kind) noexcept;

/// Tracks one job's state and refuses illegal transitions.
class Machine {
public:
    /// Starts a job: Idle -> Fetching (source=manifest) or VerifyingStaged.
    /// False if a job is already active (the API's 409 Busy).
    bool start(Kind kind, Source source);
    /// Moves to `to` if the edge is legal; false (and no change) otherwise.
    bool advance(State to);
    /// Terminal -> Idle, so the next job can start. The last job's outcome
    /// stays readable through state() until then.
    void reset();

    State state() const noexcept { return state_; }
    Kind kind() const noexcept { return kind_; }
    Source source() const noexcept { return source_; }
    /// The radio handover began (WiFi may be down until Restoring finishes).
    bool handover_started() const noexcept { return handover_; }

private:
    State state_ = State::Idle;
    Kind kind_ = Kind::Stage;
    Source source_ = Source::Manifest;
    bool handover_ = false;
};

/// POST /api/v1/targets/{id}/flash body. Every field is optional.
struct FlashRequest {
    Source source = Source::Manifest;
    uint32_t arm_timeout_s = 600;
    bool wifi_off = false;
};

/// Parses a flash request; `defaults` supplies the target's arm timeout and
/// the device's wifi_off_dfu. An empty body means all defaults. Returns ""
/// or the reason the body was refused.
std::string parse_flash_request(std::string_view body, const FlashRequest& defaults, FlashRequest& out);

/// POST /api/v1/targets/{id}/stage body: {"force":bool,"url":"..."}, both
/// optional (url overrides the target's manifest URL for this job only).
struct StageRequest {
    bool force = false;
    std::string manifest_url;
};
std::string parse_stage_request(std::string_view body, StageRequest& out);

/// SPEC.md §7.3 run deadline, armed when the radio handover starts:
/// 120 s + attempts x (60 s + bin_size / 2 kB/s), capped at `cap_s`.
uint32_t run_deadline_s(uint32_t attempts, uint32_t bin_size, uint32_t cap_s) noexcept;

inline constexpr uint32_t kDefaultDeadlineMaxS = 1200;

/// SPEC.md §7.3 layer 4 (restore verification), decided after restore
/// attempt `attempt` (1 or 2) has run its course. In order:
/// - !expect_connect -> Done: the home network was already down at the
///   handover, so a missing IP is not the restore's fault (never a reboot);
/// - fault (the `restore_fail` injection, which stands for a dead radio) ->
///   Retry, then Reboot, so the bench can exercise the reboot path;
/// - sta_ip -> Done;
/// - attempt < 2 -> Retry;
/// - radio_alive -> Done: the driver works (it reported a disconnect or
///   connect failure since the restore, or DFU32's access point is up), only
///   the home network is gone; rebooting wouldn't bring it back and would drop
///   the phones on the access point (§9.5), so keep running and let the
///   station keep retrying;
/// - otherwise Reboot: no sign of life from the driver.
enum class RestoreStep : uint8_t { Done, Retry, Reboot };
RestoreStep restore_step(bool expect_connect, bool sta_ip, bool fault, bool radio_alive, uint8_t attempt) noexcept;

/// The first-boot WiFi test of a pending OTA image (SPEC.md §10.1 rollback,
/// §12.3), decided once the 90 s window ends: it rolls back only for faults
/// the new image could have caused. In order: a station IP confirms; no stored
/// network confirms without a network test; a disconnect reason
/// (`last_reason` != 0: network not found, wrong passphrase) confirms, since the
/// radio works and the router is at fault; no station activity at all rolls
/// back. `ap_started` never confirms (§9.5): a working SoftAP proves nothing
/// about the station path, which is what a provisioned image must show.
enum class OtaVerdict : uint8_t { Confirm, ConfirmNoNetwork, ConfirmRadioWorks, Rollback };
OtaVerdict ota_verdict(bool sta_ip, bool ap_started, bool provisioned, uint8_t last_reason) noexcept;

/// What GET /api/v1/status shows about the current (or last) job.
struct Snapshot {
    uint32_t id = 0;                 // increments per job; 0 = none since boot
    State state = State::Idle;
    Kind kind = Kind::Stage;
    Source source = Source::Manifest;
    std::string target;              // target id
    std::string phase;               // finer-grained step, e.g. "SCAN", "STREAM"
    std::string version;             // image version, once known
    uint32_t attempt = 0;
    uint32_t max_attempts = 0;
    uint32_t bytes_done = 0;         // download bytes (Fetching) or bytes acked (DfuRunning)
    uint32_t bytes_total = 0;
    uint32_t rate_bps = 0;           // stream rate so far
    uint32_t started_uptime_s = 0;
    uint32_t arm_deadline_uptime_s = 0;  // AwaitingTarget gives up at this uptime
    bool wifi_off = false;           // WiFi is (or will be) stopped for the transfer
    bool target_erased = false;      // START reached: the target's app is gone until success
    bool reused = false;             // stage: staged image already matched
    std::string bootloader;          // DIS firmware revision, once connected
    std::string error;               // SPEC.md §13.3 code + detail, when Failed
    uint32_t run_seq = 0;            // run record written for this job (flash only)
};

/// Writes the snapshot's fields into `w` (the "job" object of /status).
/// `now_uptime_s` turns the arm deadline into a countdown.
void write_json(json::ObjectWriter& w, const Snapshot& s, uint32_t now_uptime_s);

}  // namespace dfu32::job
