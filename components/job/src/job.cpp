// SPDX-License-Identifier: MIT
#include "job/job.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string_view>

#include "config/config.hpp"
#include "dfu32_core/api_core.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fetch/fetch.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net/net.hpp"
#include "runlog/runlog_idf.hpp"

namespace dfu32::job {
namespace {

constexpr const char* TAG = "job";
constexpr uint32_t kTaskStack = 12288;        // HTTP client + manifest parse + BLE flash
constexpr uint32_t kRestoreWaitMs = 60000;    // SPEC.md §7.3 layer 4
constexpr int64_t kStallUs = 30LL * 1000000;  // stall watchdog (layer 3)
constexpr uint64_t kStallCheckUs = 5ULL * 1000000;

struct Request {
    Kind kind = Kind::Stage;
    Source source = Source::Manifest;
    TargetConfig target;
    FlashRequest flash;
    StageRequest stage;
    api::DeviceConfig device;
    Faults faults;
};

std::mutex g_mu;                 // guards everything below up to g_faults
Machine g_machine;
Snapshot g_snap;
const char* g_holder = nullptr;
uint32_t g_next_id = 0;
Faults g_faults;
int64_t g_stream_t0_us = 0;

QueueHandle_t g_queue = nullptr;
esp_timer_handle_t g_deadline_timer = nullptr;
esp_timer_handle_t g_stall_timer = nullptr;
std::atomic<bool> g_abort_req{false};
std::atomic<bool> g_deadline_hit{false};
std::atomic<int64_t> g_watch_since_us{0};

uint32_t uptime_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// ---- state / snapshot helpers ------------------------------------------------------

bool set_state(State to) {
    State from;
    bool ok;
    uint32_t id;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        from = g_machine.state();
        ok = g_machine.advance(to);
        if (ok) g_snap.state = to;
        id = g_snap.id;
    }
    if (ok) {
        ESP_LOGI(TAG, "evt=job id=%" PRIu32 " from=%s to=%s", id, to_string(from), to_string(to));
    } else {
        ESP_LOGE(TAG, "illegal job transition %s -> %s (bug)", to_string(from), to_string(to));
    }
    return ok;
}

template <typename F>
void update(F&& f) {
    std::lock_guard<std::mutex> lk(g_mu);
    f(g_snap);
}

void set_phase(const char* phase) {
    update([&](Snapshot& s) { s.phase = phase; });
}

void fail(const std::string& error) {
    update([&](Snapshot& s) { s.error = error; });
    ESP_LOGE(TAG, "job failed: %s", error.c_str());
    set_state(State::Failed);
}

bool aborted() { return g_abort_req.load(); }

// ---- timers (SPEC.md §7.3 layers 2 and 3) -------------------------------------------

void deadline_cb(void*) {
    ESP_LOGE(TAG, "run deadline exceeded: aborting the BLE link");
    g_deadline_hit = true;
    ble::request_abort();
}

void stall_cb(void*) {
    const int64_t last = std::max(ble::last_activity_us(), g_watch_since_us.load());
    const int64_t idle = esp_timer_get_time() - last;
    if (idle > kStallUs) {
        ESP_LOGE(TAG, "DFU task shows no sign of life for %" PRId64 " s: rebooting", idle / 1000000);
        esp_system_abort("DFU32: DFU task stalled (no BLE heartbeat for 30 s)");
    }
}

void start_stall_watch() {
    g_watch_since_us = esp_timer_get_time();
    esp_timer_start_periodic(g_stall_timer, kStallCheckUs);
}

void stop_timers() {
    esp_timer_stop(g_stall_timer);
    esp_timer_stop(g_deadline_timer);
}

// ---- WiFi restore with verification (SPEC.md §7.3 layer 4) ---------------------------

void restore_wifi(bool expect_connect, bool fault) {
    if (fault) ESP_LOGW(TAG, "fault injection: treating WiFi restore as failed");
    bool ok = net::restore(kRestoreWaitMs) && !fault;
    if (ok) {
        ESP_LOGI(TAG, "WiFi restored");
        return;
    }
    if (!expect_connect) {
        ESP_LOGW(TAG, "WiFi restarted; it was not connected before the transfer either, so not insisting");
        return;
    }
    ESP_LOGW(TAG, "WiFi not connected %" PRIu32 " s after restore; retrying once", kRestoreWaitMs / 1000);
    net::stop();
    ok = net::restore(kRestoreWaitMs) && !fault;
    if (ok) {
        ESP_LOGI(TAG, "WiFi restored on the second try");
        return;
    }
    ESP_LOGE(TAG, "WiFi still down after a retry: rebooting so the network comes back (SPEC.md §7.3)");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

// ---- the flash part of a job -----------------------------------------------------------

void run_flash(const Request& q, StagedImage& img, const StagingVerify& v) {
    ble::Mac mac;
    ble::Mac::parse(q.target.mac, q.target.addr_type, mac);  // validated at submit
    const uint32_t bin_size = img.header().bin_size;

    runlog::RunLog* run_log = runlog::idf::open_run_log();
    RunContext ctx;
    ctx.seq = run_log ? run_log->next_seq() : 0;
    ctx.target_id = q.target.effective_id();
    ctx.started_uptime_s = uptime_s();
    ctx.started_unix = net::unix_time();
    ctx.trigger = "api";
    ctx.source = to_string(q.source);
    const uint64_t log_mark = runlog::idf::log_position();
    if (run_log != nullptr) {
        runlog::idf::JobMarker m;
        m.seq = ctx.seq;
        m.started_uptime_s = ctx.started_uptime_s;
        std::snprintf(m.target, sizeof(m.target), "%s", mac.str().c_str());
        std::snprintf(m.version, sizeof(m.version), "%s", img.header().version.c_str());
        runlog::idf::job_begin(m);
    }

    update([&](Snapshot& s) {
        s.phase = "SCAN";
        s.arm_deadline_uptime_s = uptime_s() + q.flash.arm_timeout_s;
        s.bytes_done = 0;
        s.bytes_total = bin_size;
        s.wifi_off = q.flash.wifi_off;
    });
    ESP_LOGI(TAG, "armed: waiting up to %" PRIu32 " s for %s (run `start ota` on the repeater)",
             q.flash.arm_timeout_s, mac.str().c_str());

    bool wifi_stopped = false;
    bool wifi_was_connected = false;
    ble::FlashOptions o;
    o.scan_ms = q.flash.arm_timeout_s * 1000;
    o.prn = q.target.prn;
    o.start_settle_ms = q.target.start_settle_ms;
    o.prn_timeout_ms = q.target.prn_timeout_ms;
    o.attempts = static_cast<uint8_t>(q.target.max_attempts);
    o.before_transfer = [&](uint8_t) {
        if (run_log != nullptr) runlog::idf::job_set_stage(runlog::idf::JobStage::Transferring);
        update([](Snapshot& s) { s.target_erased = true; });  // START may erase from here on
    };
    o.on_found = [&] {
        set_state(State::RadioHandover);
        set_phase("HANDOVER");
        const net::Status ns = net::status();
        if (q.flash.wifi_off && ns.initialised && !ns.stopped) {
            wifi_was_connected = ns.connected;
            wifi_stopped = net::stop();
        }
        update([&](Snapshot& s) { s.wifi_off = wifi_stopped; });
        const uint32_t deadline =
            q.faults.deadline_s ? q.faults.deadline_s
                                : run_deadline_s(o.attempts, bin_size, q.device.dfu_deadline_max_s);
        ESP_LOGI(TAG, "target seen; WiFi %s, run deadline %" PRIu32 " s", wifi_stopped ? "stopped" : "kept up",
                 deadline);
        esp_timer_start_once(g_deadline_timer, uint64_t{deadline} * 1000000);
        if (q.faults.stall_s) {
            ESP_LOGW(TAG, "fault injection: blocking the DFU task %" PRIu32 " s without a heartbeat", q.faults.stall_s);
            vTaskDelay(pdMS_TO_TICKS(q.faults.stall_s * 1000));
        }
        set_state(State::DfuRunning);
    };
    o.on_progress = [&](const ble::FlashProgress& p) {
        const int64_t now = esp_timer_get_time();
        std::lock_guard<std::mutex> lk(g_mu);
        g_snap.attempt = p.attempt;
        g_snap.phase = p.phase;
        g_snap.bytes_done = p.bytes_acked;
        g_snap.bytes_total = p.bytes_total;
        if (std::string_view(p.phase) == "STREAM") {
            if (g_stream_t0_us == 0 || p.bytes_acked == 0) g_stream_t0_us = now;
            const int64_t dt = now - g_stream_t0_us;
            g_snap.rate_bps = dt > 0 ? static_cast<uint32_t>(int64_t{p.bytes_acked} * 1000000 / dt) : 0;
        } else {
            g_stream_t0_us = 0;
        }
    };

    start_stall_watch();
    const ble::FlashReport r = ble::flash(mac, img, bin_size, img.header().dat_bytes(), v.init, o);
    stop_timers();

    bool handover;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        handover = g_machine.handover_started();
        g_snap.bootloader = r.link.dis_fw_rev;
        g_snap.target_erased = r.target_erased;
    }

    std::string error;
    if (r.ok) {
        error.clear();
    } else if (g_deadline_hit) {
        error = "DEADLINE_EXCEEDED: the run took longer than its deadline; BLE link aborted";
    } else if (r.aborted) {
        error = "ABORTED";
    } else if (!handover && starts_with(r.error, "target not found")) {
        error = "TARGET_NOT_FOUND: no advert from " + mac.str() + " or MAC+1 within " +
                std::to_string(q.flash.arm_timeout_s) + " s (run `start ota` on the repeater)";
    } else {
        error = r.error;
    }

    if (handover) {
        set_state(State::Restoring);
        set_phase("RESTORE");
    }

    ctx.wifi_off = wifi_stopped;
    uint32_t recorded = 0;
    if (run_log != nullptr) {
        runlog::RunRecord rec = make_run_record(ctx, mac, img, r);
        rec.error = error;
        recorded = run_log->append(runlog::to_json(rec), runlog::idf::log_since(log_mark, runlog::kSlotPayloadMax));
        runlog::idf::job_end();
    }
    update([&](Snapshot& s) { s.run_seq = recorded; });

    if (r.ok) {
        TargetConfig t;
        if (config::load_target(t) && t.effective_id() == q.target.effective_id()) {
            t.last_ok_version = img.header().version;
            t.last_ok_at = net::unix_time();
            if (!config::save_target(t)) ESP_LOGW(TAG, "could not record last_ok_version");
        }
    }

    if (wifi_stopped) restore_wifi(wifi_was_connected, q.faults.restore_fail);

    if (r.ok) {
        ESP_LOGI(TAG, "flash of \"%s\" succeeded in %" PRIu32 " ms (run #%" PRIu32 ")", img.header().version.c_str(),
                 r.total_ms, recorded);
        set_phase("DONE");
        set_state(State::Succeeded);
    } else {
        fail(error);
    }
}

// ---- one job ---------------------------------------------------------------------------

void run(const Request& q) {
    State st;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        st = g_machine.state();
    }

    if (st == State::Fetching) {
        set_phase("FETCH");
        if (!net::status().connected) {
            fail("HTTP_ERROR: WiFi is not connected");
            return;
        }
        fetch::StageOptions o;
        o.force = q.stage.force;
        o.staged_at = net::unix_time();
        o.progress = [](uint32_t done, uint32_t total) {
            update([&](Snapshot& s) {
                s.bytes_done = done;
                s.bytes_total = total;
            });
        };
        o.cancelled = [] { return aborted(); };
        fetch::StageReport rep;
        const Status s = fetch::stage(q.target, q.stage.manifest_url, o, rep);
        update([&](Snapshot& sn) {
            sn.version = rep.manifest.version;
            sn.reused = rep.reused;
        });
        if (!s.ok()) {
            fail(s.str() + (rep.invalidated ? " (the previously staged image was invalidated)" : ""));
            return;
        }
        set_state(State::VerifyingStaged);
    }
    if (aborted()) {
        fail("ABORTED");
        return;
    }

    set_phase("VERIFY");
    StagedImage img;
    if (!img.open()) {
        fail("STAGING_IO: no staging partition");
        return;
    }
    if (img.header_status() != dfu::StagingHeaderError::Ok) {
        fail(std::string("NO_STAGED_IMAGE: ") + dfu::to_string(img.header_status()));
        return;
    }
    update([&](Snapshot& s) {
        s.version = img.header().version;
        s.bytes_total = img.header().bin_size;
    });
    const StagingVerify v = img.verify(q.target.max_app_size);
    if (!v.ok) {
        fail(v.error);
        return;
    }
    set_state(State::Precheck);

    set_phase("PRECHECK");
    if (!q.target.board.empty() && !iequals(img.header().board, q.target.board)) {
        fail("BOARD_MISMATCH: staged image is for board \"" + img.header().board + "\", target is \"" +
             q.target.board + "\"");
        return;
    }
    if (q.kind == Kind::Stage) {
        set_phase("DONE");
        set_state(State::Staged);
        ESP_LOGI(TAG, "staged \"%s\" for '%s'", img.header().version.c_str(), q.target.effective_id().c_str());
        return;
    }
    if (aborted()) {
        fail("ABORTED");
        return;
    }
    set_state(State::AwaitingTarget);
    run_flash(q, img, v);
}

void job_task(void*) {
    for (;;) {
        Request* raw = nullptr;
        if (xQueueReceive(g_queue, &raw, portMAX_DELAY) != pdTRUE || raw == nullptr) continue;
        std::unique_ptr<Request> q(raw);
        run(*q);
        std::lock_guard<std::mutex> lk(g_mu);
        if (is_active(g_machine.state())) {
            // Every path above ends in a terminal state; this is a safety net.
            ESP_LOGE(TAG, "job ended in %s; marking it failed", to_string(g_machine.state()));
            g_snap.state = State::Failed;
            if (g_snap.error.empty()) g_snap.error = "internal error";
            g_machine = Machine{};
        }
        g_holder = nullptr;
    }
}

SubmitResult submit(std::unique_ptr<Request> q, std::string& why, uint32_t& job_id) {
    if (g_queue == nullptr) {
        why = "job task not running";
        return SubmitResult::Invalid;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_holder != nullptr) {
            why = std::string("busy (") + g_holder + ")";
            return SubmitResult::Busy;
        }
        g_holder = "job";
        g_machine.reset();
        g_machine.start(q->kind, q->source);
        if (q->kind == Kind::Flash) {
            q->faults = g_faults;
            g_faults = Faults{};
        }
        g_snap = Snapshot{};
        g_snap.id = ++g_next_id;
        g_snap.state = g_machine.state();
        g_snap.kind = q->kind;
        g_snap.source = g_machine.source();
        g_snap.target = q->target.effective_id();
        g_snap.started_uptime_s = uptime_s();
        g_snap.max_attempts = q->target.max_attempts;
        g_snap.wifi_off = q->kind == Kind::Flash && q->flash.wifi_off;
        g_stream_t0_us = 0;
        job_id = g_snap.id;
    }
    g_abort_req = false;
    g_deadline_hit = false;
    ble::clear_abort();
    ESP_LOGI(TAG, "evt=job id=%" PRIu32 " start kind=%s source=%s target=%s", job_id, to_string(q->kind),
             to_string(q->source), q->target.effective_id().c_str());
    Request* raw = q.release();
    if (xQueueSend(g_queue, &raw, 0) != pdTRUE) {  // can't happen: one job at a time
        delete raw;
        std::lock_guard<std::mutex> lk(g_mu);
        g_holder = nullptr;
        g_machine = Machine{};
        g_snap.state = State::Idle;
        why = "job queue full";
        return SubmitResult::Busy;
    }
    return SubmitResult::Ok;
}

std::string check_fetchable(const TargetConfig& t, const std::string& url_override) {
    if (t.board.empty()) return "the target has no board set (matched against the manifest's \"boards\")";
    if (t.manifest_url.empty() && url_override.empty()) return "the target has no manifest_url";
    return {};
}

}  // namespace

bool init() {
    if (g_queue != nullptr) return true;
    esp_timer_create_args_t dl{};
    dl.callback = &deadline_cb;
    dl.name = "job_deadline";
    esp_timer_create_args_t st{};
    st.callback = &stall_cb;
    st.name = "job_stall";
    if (esp_timer_create(&dl, &g_deadline_timer) != ESP_OK || esp_timer_create(&st, &g_stall_timer) != ESP_OK) {
        ESP_LOGE(TAG, "timer create failed");
        return false;
    }
    g_queue = xQueueCreate(1, sizeof(Request*));
    if (g_queue == nullptr || xTaskCreate(&job_task, "job", kTaskStack, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "job task create failed");
        return false;
    }
    return true;
}

bool try_acquire(const char* who) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_holder != nullptr) return false;
    g_holder = who;
    return true;
}

void release() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_holder = nullptr;
}

const char* holder() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_holder;
}

SubmitResult submit_stage(const TargetConfig& target, const StageRequest& req, std::string& why, uint32_t& job_id) {
    why = check_fetchable(target, req.manifest_url);
    if (!why.empty()) return SubmitResult::Invalid;
    auto q = std::make_unique<Request>();
    q->kind = Kind::Stage;
    q->target = target;
    q->stage = req;
    config::load_device(q->device);
    return submit(std::move(q), why, job_id);
}

SubmitResult submit_flash(const TargetConfig& target, const FlashRequest& req, std::string& why, uint32_t& job_id) {
    ble::Mac mac;
    if (target.mac.empty() || !ble::Mac::parse(target.mac, target.addr_type, mac)) {
        why = "the target has no MAC (set mac, e.g. from POST /api/v1/ble/scan)";
        return SubmitResult::Invalid;
    }
    if (req.source == Source::Manifest) {
        why = check_fetchable(target, {});
        if (!why.empty()) return SubmitResult::Invalid;
    }
    auto q = std::make_unique<Request>();
    q->kind = Kind::Flash;
    q->source = req.source;
    q->target = target;
    q->flash = req;
    config::load_device(q->device);
    return submit(std::move(q), why, job_id);
}

AbortResult abort() {
    AbortResult r;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        r.state = g_snap.state;
        r.was_active = is_active(g_snap.state);
        r.target_erased = g_snap.target_erased;
    }
    if (r.was_active) {
        ESP_LOGW(TAG, "abort requested in %s", to_string(r.state));
        g_abort_req = true;
        ble::request_abort();
    }
    return r;
}

Snapshot snapshot() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_snap;
}

bool busy() {
    std::lock_guard<std::mutex> lk(g_mu);
    return is_active(g_machine.state());
}

void set_faults(const Faults& f) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_faults = f;
}

Faults faults() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_faults;
}

// ---- run records -------------------------------------------------------------------------

namespace {

void hex(const uint8_t* p, size_t n, char* out) {
    for (size_t i = 0; i < n; ++i) std::snprintf(out + 2 * i, 3, "%02x", p[i]);
}

uint32_t stream_rate_x10(const dfu::TransferResult& t) {
    const uint32_t ms = t.phase_ms[static_cast<size_t>(dfu::TransferPhase::Stream)];
    return ms ? static_cast<uint32_t>(uint64_t{t.bytes_sent} * 10 / ms) : 0;
}

}  // namespace

runlog::RunRecord make_run_record(const RunContext& ctx, const ble::Mac& mac, const StagedImage& img,
                                  const ble::FlashReport& r) {
    runlog::RunRecord rec;
    rec.seq = ctx.seq;
    rec.result = r.ok ? "SUCCEEDED" : "FAILED";
    rec.error = r.error;
    rec.failure_kind = dfu::to_string(r.failure);
    rec.target = mac.str();
    rec.target_id = ctx.target_id;
    rec.trigger = ctx.trigger;
    rec.source = ctx.source;
    rec.started_unix = ctx.started_unix;
    rec.image_version = img.header().version;
    char sha[65] = {};
    hex(img.header().bin_sha256.data(), 32, sha);
    rec.image_sha256 = sha;
    rec.image_size = img.header().bin_size;
    rec.started_uptime_s = ctx.started_uptime_s;
    rec.duration_ms = r.total_ms;
    bool any_transfer = false;
    for (const auto& a : r.attempts) any_transfer = any_transfer || a.ran_transfer;
    if (any_transfer) rec.last_phase = dfu::to_string(r.transfer.phase);
    rec.bytes_acked = r.transfer.bytes_acked;
    rec.target_erased = r.target_erased ? "yes" : "no";
    rec.sys_reset_after_failure = r.sys_reset_after_failure;
    rec.wifi_off = ctx.wifi_off;
    rec.bootloader = r.link.dis_fw_rev;
    rec.bootloader_class = dfu::to_string(r.link.bootloader.cls);
    rec.mtu = r.link.mtu;
    rec.packet_size = r.link.packet_size;
    rec.conn_interval_x125 = r.link.conn_interval;
    rec.rate_x10_kBps = stream_rate_x10(r.transfer);
    for (const auto& a : r.attempts) {
        runlog::RunAttempt ra;
        ra.number = a.number;
        ra.interval_ms = a.params.interval_ms;
        ra.prn = a.params.prn;
        ra.outcome = a.failure == dfu::FailureKind::None ? "OK" : a.error;
        ra.kind = dfu::to_string(a.failure);
        ra.bytes_acked = a.transfer.bytes_acked;
        rec.attempts.push_back(std::move(ra));
    }
    return rec;
}

}  // namespace dfu32::job
