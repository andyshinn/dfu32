// SPDX-License-Identifier: MIT
#include "api/api.hpp"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "dfu32_core/api_core.hpp"
#include "dfu32_core/job.hpp"
#include "dfu32_core/json.hpp"
#include "dfu32_core/manifest.hpp"
#include "dfu32_core/ota_image.hpp"
#include "dfu_ble/dfu_ble.hpp"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fetch/fetch.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "job/job.hpp"
#include "net/net.hpp"
#include "psa/crypto.h"
#include "runlog/run_record.hpp"
#include "runlog/runlog_idf.hpp"
#include "sdkconfig.h"
#include "staging/staging.hpp"
#include "staging/staging_io.hpp"

// DFU32's custom app descriptor (SPEC.md §10.1, firmware upload): the board
// profile this image was built for, right after esp_app_desc_t in the image.
// POST /api/v1/system/ota compares an upload's tag with this one, which also
// keeps the linker from dropping the section.
__attribute__((section(".rodata_custom_desc"), used)) const dfu32::ota::BoardTag dfu32_board_tag = {
    {'D', 'F', '3', '2', 'B', 'R', 'D', '1'}, CONFIG_DFU32_BOARD_NAME};

namespace dfu32::api {
namespace {

constexpr const char* TAG = "api";
constexpr size_t kMaxBody = 4096;
constexpr size_t kLogChunk = 8192;
constexpr int kMaxRecvTimeouts = 2;       // x recv_wait_timeout (5 s)
constexpr uint32_t kSlowTaskStack = 12288;  // check runs the HTTP client + manifest parser
constexpr const char* kDfuUuid = "00001530-1212-efde-1523-785feabcd123";

httpd_handle_t g_server = nullptr;

// Token state, cached from NVS (only the SHA-256 is ever stored).
std::mutex g_auth_mu;
bool g_claimed = false;
std::array<uint8_t, 32> g_token_hash{};
AuthThrottle g_throttle;

uint32_t uptime_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }
uint64_t now_ms() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

// ---- responses ---------------------------------------------------------------------

esp_err_t send_json(httpd_req_t* req, const char* status, const std::string& body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
}

esp_err_t send_obj(httpd_req_t* req, const char* status, const json::ObjectWriter& w) {
    const std::string body = w.str();
    if (body.empty()) return send_json(req, "500 Internal Server Error", R"({"error":"INTERNAL","detail":"out of memory"})");
    return send_json(req, status, body);
}

esp_err_t send_error(httpd_req_t* req, const char* status, const char* code, const std::string& detail = {}) {
    json::ObjectWriter w;
    w.add("error", code);
    if (!detail.empty()) w.add("detail", detail);
    return send_obj(req, status, w);
}

esp_err_t send_no_content(httpd_req_t* req) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
}

/// Maps a dfu32::Status from fetch to an HTTP error.
esp_err_t send_status_error(httpd_req_t* req, const Status& s) {
    const char* http = s.code == Err::HttpError ? "502 Bad Gateway" : "422 Unprocessable Content";
    return send_error(req, http, to_string(s.code), s.detail);
}

std::string header(httpd_req_t* req, const char* name) {
    const size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0) return {};
    std::string v(len + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, name, v.data(), v.size()) != ESP_OK) return {};
    v.resize(len);
    return v;
}

/// Reads the request body (bodies are optional: none means "defaults").
/// Sends the error response itself and returns false when it can't.
bool read_body(httpd_req_t* req, std::string& out, esp_err_t& sent) {
    out.clear();
    // esp_http_server doesn't decode chunked request bodies (content_len
    // stays 0), so refuse them rather than silently using the defaults.
    if (httpd_req_get_hdr_value_len(req, "Transfer-Encoding") != 0) {
        sent = send_error(req, "411 Length Required", "BAD_REQUEST", "send the body with Content-Length, not chunked");
        return false;
    }
    if (req->content_len > kMaxBody) {
        sent = send_error(req, "413 Content Too Large", "BAD_REQUEST", "bodies are limited to 4 KB");
        return false;
    }
    out.resize(req->content_len);
    size_t got = 0;
    int timeouts = 0;
    while (got < out.size()) {
        const int n = httpd_req_recv(req, out.data() + got, out.size() - got);
        // Each timeout is recv_wait_timeout (5 s); a client that stalls longer
        // must not hold the server's only worker thread.
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < kMaxRecvTimeouts) continue;
        if (n <= 0) {
            sent = send_error(req, "408 Request Timeout", "BAD_REQUEST", "incomplete request body");
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

// ---- auth ----------------------------------------------------------------------------

bool sha256(std::string_view text, std::array<uint8_t, 32>& out) {
    size_t len = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(text.data()), text.size(), out.data(),
                            out.size(), &len) == PSA_SUCCESS &&
           len == out.size();
}

void load_token() {
    std::lock_guard<std::mutex> lk(g_auth_mu);
    g_claimed = config::load_token_hash(g_token_hash);
}

bool claimed() {
    std::lock_guard<std::mutex> lk(g_auth_mu);
    return g_claimed;
}

/// New random token; stores its hash. Empty on failure.
std::string issue_token() {
    std::array<uint8_t, 32> raw{};
    esp_fill_random(raw.data(), raw.size());
    const std::string token = base64url_encode(raw);
    std::array<uint8_t, 32> hash{};
    if (!sha256(token, hash) || !config::save_token_hash(hash)) return {};
    std::lock_guard<std::mutex> lk(g_auth_mu);
    g_token_hash = hash;
    g_claimed = true;
    return token;
}

/// Checks the bearer token; sends the error response itself when refusing.
bool authorize(httpd_req_t* req, esp_err_t& sent) {
    std::array<uint8_t, 32> want{};
    {
        std::lock_guard<std::mutex> lk(g_auth_mu);
        if (!g_claimed) {
            sent = send_error(req, "403 Forbidden", "UNCLAIMED", "claim the device first: POST /api/v1/claim");
            return false;
        }
        // After repeated failures, check at most one token per second and
        // refuse the rest at once (never sleep on the server thread).
        if (!g_throttle.try_attempt(now_ms())) {
            httpd_resp_set_hdr(req, "Retry-After", "1");
            sent = send_error(req, "429 Too Many Requests", "RATE_LIMITED",
                              "too many failed token attempts; retry in a second");
            return false;
        }
        want = g_token_hash;
    }
    const std::string hdr = header(req, "Authorization");
    const std::string_view token = bearer_token(hdr);
    std::array<uint8_t, 32> got{};
    if (!token.empty() && sha256(token, got) && ct_equal(got, want)) return true;
    {
        std::lock_guard<std::mutex> lk(g_auth_mu);
        g_throttle.record_failure(now_ms());
    }
    ESP_LOGW(TAG, "rejected request to %s: %s", req->uri, token.empty() ? "no bearer token" : "wrong token");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
    sent = send_error(req, "401 Unauthorized", "UNAUTHORIZED", "Authorization: Bearer <token> required");
    return false;
}

// ---- shared views --------------------------------------------------------------------

std::string busy_detail() {
    const char* h = job::holder();
    return std::string("busy: ") + (h != nullptr ? h : "another operation") + " is using BLE/staging";
}

bool target_exists(const TargetConfig& t) {
    return !(t.id.empty() && t.mac.empty() && t.board.empty() && t.manifest_url.empty());
}

/// Loads the target and checks it matches {id}; sends 404 otherwise.
bool load_target_for(httpd_req_t* req, const std::string& id, TargetConfig& t, esp_err_t& sent) {
    if (!config::load_target(t)) {
        sent = send_error(req, "500 Internal Server Error", "INTERNAL", "cannot read settings");
        return false;
    }
    if (!target_exists(t) || t.effective_id() != id) {
        sent = send_error(req, "404 Not Found", "NOT_FOUND", "no target '" + id + "'");
        return false;
    }
    return true;
}

void write_staged(json::ObjectWriter& w) {
    StagedImage img;
    if (!img.open() || img.header_status() != dfu::StagingHeaderError::Ok) {
        w.add_null("staged");
        return;
    }
    const auto& h = img.header();
    json::ObjectWriter s;
    s.add("target", h.target_id)
        .add("board", h.board)
        .add("version", h.version)
        .add("bin_size", int64_t{h.bin_size})
        .add("sha256", sha256_hex(h.bin_sha256))
        .add("staged_at", static_cast<int64_t>(h.staged_at))
        .add("manifest_url", h.manifest_url);
    w.add("staged", std::move(s));
}

bool run_matches(const std::string& json, const TargetConfig& t) {
    const std::string id = runlog::json_get_string(json, "target_id");
    if (!id.empty()) return id == t.effective_id();
    return !t.mac.empty() && runlog::json_get_string(json, "target") == t.mac;  // pre-M3b records
}

// ---- handlers ------------------------------------------------------------------------

esp_err_t h_status(httpd_req_t* req) {
    const esp_app_desc_t* app = esp_app_get_description();
    const net::Status ns = net::status();
    json::ObjectWriter dev;
    dev.add("name", "DFU32")
        .add("version", app->version)
        .add("board", CONFIG_DFU32_BOARD_NAME)
        .add("uptime_s", int64_t{uptime_s()})
        .add_bool("claimed", claimed())
        .add("hostname", ns.hostname)
        .add("unix_time", static_cast<int64_t>(net::unix_time()));
    json::ObjectWriter wifi;
    wifi.add_bool("connected", ns.connected).add("rssi", int64_t{ns.rssi}).add("ip", ns.ip);
    dev.add("wifi", std::move(wifi));
    if (const char* h = job::holder()) {
        dev.add("busy", h);
    } else {
        dev.add_null("busy");
    }

    json::ObjectWriter w;
    w.add("device", std::move(dev));
    json::ObjectWriter jb;
    write_json(jb, job::snapshot(), uptime_s());
    w.add("job", std::move(jb));

    json::ArrayWriter targets;
    TargetConfig t;
    if (config::load_target(t) && target_exists(t)) {
        json::ObjectWriter o;
        o.add("id", t.effective_id())
            .add("name", t.name)
            .add("board", t.board)
            .add("last_ok_version", t.last_ok_version)
            .add("last_ok_at", static_cast<int64_t>(t.last_ok_at));
        targets.push(std::move(o));
    }
    w.add("targets", std::move(targets));
    write_staged(w);
    return send_obj(req, "200 OK", w);
}

esp_err_t h_claim(httpd_req_t* req) {
    if (claimed()) return send_error(req, "409 Conflict", "ALREADY_CLAIMED", "use the token issued at claim time");
    const std::string token = issue_token();
    if (token.empty()) return send_error(req, "500 Internal Server Error", "INTERNAL", "could not store the token");
    ESP_LOGI(TAG, "device claimed");
    json::ObjectWriter w;
    w.add("token", token);
    return send_obj(req, "200 OK", w);
}

esp_err_t h_targets(httpd_req_t* req) {
    TargetConfig t;
    if (!config::load_target(t)) return send_error(req, "500 Internal Server Error", "INTERNAL");
    json::ArrayWriter arr;
    if (target_exists(t)) {
        json::ObjectWriter o;
        write_json(o, t);
        o.add("id", t.effective_id());
        arr.push(std::move(o));
    }
    json::ObjectWriter w;
    w.add("targets", std::move(arr));
    return send_obj(req, "200 OK", w);
}

esp_err_t h_target(httpd_req_t* req, const RouteMatch& m) {
    esp_err_t sent = ESP_OK;
    if (req->method == HTTP_GET) {
        TargetConfig t;
        if (!load_target_for(req, m.id, t, sent)) return sent;
        json::ObjectWriter w;
        write_json(w, t);
        w.add("id", t.effective_id());
        runlog::RunLog* rl = runlog::idf::open_run_log();
        bool found = false;
        if (rl != nullptr) {
            for (const auto& slot : rl->list()) {
                if (run_matches(slot.json, t)) {
                    w.add_json("last_run", slot.json);
                    found = true;
                    break;
                }
            }
        }
        if (!found) w.add_null("last_run");
        return send_obj(req, "200 OK", w);
    }

    // Hold the device-wide lease while editing, so the target can't change
    // under a job, a console flash/probe or a scan (and a flash can't start
    // between this check and the save).
    job::Lease lease("config");
    if (!lease.ok()) return send_error(req, "409 Conflict", "BUSY", busy_detail());
    TargetConfig t;
    if (!config::load_target(t)) return send_error(req, "500 Internal Server Error", "INTERNAL");
    const bool exists = target_exists(t);

    if (req->method == HTTP_DELETE) {
        if (!exists || t.effective_id() != m.id) return send_error(req, "404 Not Found", "NOT_FOUND");
        if (!config::erase_target()) return send_error(req, "500 Internal Server Error", "INTERNAL");
        return send_no_content(req);
    }

    // PUT: create or update. v1 has one target; an unnamed (console-made)
    // target adopts the first id it is PUT under.
    if (exists && !t.id.empty() && t.id != m.id) {
        return send_error(req, "409 Conflict", "TARGET_LIMIT",
                          "v1 supports one target ('" + t.id + "'); DELETE it first or PUT to its id");
    }
    std::string body;
    esp_err_t body_err = ESP_OK;
    if (!read_body(req, body, body_err)) return body_err;
    if (!exists) t = TargetConfig{};
    if (auto e = apply_json(body, m.id, t); !e.empty()) return send_error(req, "400 Bad Request", "BAD_REQUEST", e);
    if (!config::save_target(t)) return send_error(req, "500 Internal Server Error", "INTERNAL", "NVS write failed");
    json::ObjectWriter w;
    write_json(w, t);
    return send_obj(req, exists ? "200 OK" : "201 Created", w);
}

esp_err_t h_check(httpd_req_t* req, const RouteMatch& m) {
    esp_err_t sent = ESP_OK;
    TargetConfig t;
    if (!load_target_for(req, m.id, t, sent)) return sent;
    if (t.manifest_url.empty()) return send_error(req, "400 Bad Request", "BAD_REQUEST", "target has no manifest_url");
    if (!net::status().connected) return send_error(req, "503 Service Unavailable", "HTTP_ERROR", "WiFi is not connected");
    fetch::CheckReport r;
    Status s;
    {
        // The lease keeps a stage job from rewriting the staging header while
        // check compares against it.
        job::Lease lease("check");
        if (!lease.ok()) return send_error(req, "409 Conflict", "BUSY", busy_detail());
        s = fetch::check(t.manifest_url, t.board, r);
    }
    if (!s.ok()) return send_status_error(req, s);
    const Manifest& mf = r.manifest;
    json::ObjectWriter w;
    w.add("version", mf.version).add("name", mf.name).add("created", mf.created).add("notes", mf.notes);
    json::ArrayWriter boards;
    for (const auto& b : mf.boards) boards.push(b);
    w.add("boards", std::move(boards)).add("target_board", t.board).add_bool("board_ok", r.board_ok);
    json::ObjectWriter bin;
    bin.add("url", mf.bin.url).add("size", int64_t{mf.bin.size}).add("sha256", sha256_hex(mf.bin.sha256));
    w.add("bin", std::move(bin));
    json::ObjectWriter dat;
    dat.add("url", mf.dat.url).add("size", int64_t{mf.dat.size});
    w.add("dat", std::move(dat));
    w.add("manifest_sha256", sha256_hex(r.manifest_sha256))
        .add_bool("staged_same", r.staged_same)
        .add("staged_version", r.staged_valid ? r.staged.version : std::string())
        .add("last_ok_version", t.last_ok_version)
        .add_bool("update_available", !mf.version.empty() && mf.version != t.last_ok_version);
    return send_obj(req, "200 OK", w);
}

esp_err_t send_submit(httpd_req_t* req, job::SubmitResult res, const std::string& why, uint32_t id) {
    if (res == job::SubmitResult::Busy) return send_error(req, "409 Conflict", "BUSY", why);
    if (res == job::SubmitResult::Invalid) return send_error(req, "400 Bad Request", "BAD_REQUEST", why);
    json::ObjectWriter w;
    w.add("job", int64_t{id}).add("state", to_string(job::snapshot().state)).add("poll", "/api/v1/status");
    return send_obj(req, "202 Accepted", w);
}

esp_err_t h_stage(httpd_req_t* req, const RouteMatch& m) {
    esp_err_t sent = ESP_OK;
    TargetConfig t;
    if (!load_target_for(req, m.id, t, sent)) return sent;
    std::string body;
    esp_err_t body_err = ESP_OK;
    if (!read_body(req, body, body_err)) return body_err;
    job::StageRequest sr;
    if (auto e = job::parse_stage_request(body, sr); !e.empty()) {
        return send_error(req, "400 Bad Request", "BAD_REQUEST", e);
    }
    std::string why;
    uint32_t id = 0;
    const auto res = job::submit_stage(t, sr, why, id);
    return send_submit(req, res, why, id);
}

esp_err_t h_flash(httpd_req_t* req, const RouteMatch& m) {
    esp_err_t sent = ESP_OK;
    TargetConfig t;
    if (!load_target_for(req, m.id, t, sent)) return sent;
    std::string body;
    esp_err_t body_err = ESP_OK;
    if (!read_body(req, body, body_err)) return body_err;
    DeviceConfig dev;
    config::load_device(dev);
    job::FlashRequest def;
    def.arm_timeout_s = t.arm_timeout_s;
    def.wifi_off = dev.wifi_off_dfu;
    job::FlashRequest fr;
    if (auto e = job::parse_flash_request(body, def, fr); !e.empty()) {
        return send_error(req, "400 Bad Request", "BAD_REQUEST", e);
    }
    std::string why;
    uint32_t id = 0;
    const auto res = job::submit_flash(t, fr, why, id);
    return send_submit(req, res, why, id);
}

esp_err_t h_abort(httpd_req_t* req) {
    const job::AbortResult r = job::abort();
    if (!r.was_active) return send_error(req, "409 Conflict", "NO_JOB", "no job is running");
    json::ObjectWriter w;
    w.add_bool("aborting", true).add("state", to_string(r.state)).add_bool("target_erased", r.target_erased);
    if (r.target_erased) {
        w.add("warning",
              "The transfer had started, so the target's app is erased. It waits in its bootloader (usually at "
              "MAC+1): flash again with {\"source\":\"staged\"} to recover.");
    }
    return send_obj(req, "202 Accepted", w);
}

esp_err_t h_target_runs(httpd_req_t* req, const RouteMatch& m) {
    esp_err_t sent = ESP_OK;
    TargetConfig t;
    if (!load_target_for(req, m.id, t, sent)) return sent;
    runlog::RunLog* rl = runlog::idf::open_run_log();
    json::ArrayWriter arr;
    if (rl != nullptr) {
        for (const auto& slot : rl->list()) {
            if (run_matches(slot.json, t)) arr.push_json(slot.json);
        }
    }
    json::ObjectWriter w;
    w.add("runs", std::move(arr));
    return send_obj(req, "200 OK", w);
}

esp_err_t h_run(httpd_req_t* req, const RouteMatch& m) {
    runlog::RunLog* rl = runlog::idf::open_run_log();
    runlog::RunSlot slot;
    if (rl == nullptr || !rl->get(m.seq, slot)) {
        return send_error(req, "404 Not Found", "NOT_FOUND", "run not found (rotated out, or never recorded)");
    }
    json::ObjectWriter w;
    w.add_json("run", slot.json).add("log", slot.log);
    return send_obj(req, "200 OK", w);
}

esp_err_t h_staging(httpd_req_t* req) {
    if (req->method == HTTP_DELETE) {
        job::Lease lease("staging");
        if (!lease.ok()) return send_error(req, "409 Conflict", "BUSY", busy_detail());
        if (!invalidate_staging()) return send_error(req, "500 Internal Server Error", "STAGING_IO");
        return send_no_content(req);
    }
    StagedImage img;
    if (!img.open()) return send_error(req, "500 Internal Server Error", "STAGING_IO", "no staging partition");
    json::ObjectWriter w;
    const bool valid = img.header_status() == dfu::StagingHeaderError::Ok;
    w.add_bool("valid", valid).add("header", dfu::to_string(img.header_status()));
    if (valid) {
        const auto& h = img.header();
        w.add("target", h.target_id)
            .add("board", h.board)
            .add("version", h.version)
            .add("bin_size", int64_t{h.bin_size})
            .add("sha256", sha256_hex(h.bin_sha256))
            .add("dat_size", int64_t{h.dat_size})
            .add("staged_at", static_cast<int64_t>(h.staged_at))
            .add("manifest_url", h.manifest_url)
            .add("manifest_sha256", sha256_hex(h.manifest_sha256));
        if (query_param(req->uri, "verify") == "1") {
            job::Lease lease("verify");  // don't re-read while a job rewrites it
            if (!lease.ok()) return send_error(req, "409 Conflict", "BUSY", busy_detail());
            const StagingVerify v = img.verify();
            w.add_bool("verified", v.ok).add("verify_error", v.error);
        }
    }
    return send_obj(req, "200 OK", w);
}

esp_err_t h_scan(httpd_req_t* req, const ScanRequest& sr) {
    TargetConfig t;
    config::load_target(t);
    std::vector<ble::Advert> list;
    {
        job::Lease lease("scan");
        if (!lease.ok()) return send_error(req, "409 Conflict", "BUSY", busy_detail());
        list = ble::scan(sr.duration_s * 1000);
    }
    json::ArrayWriter arr;
    for (const auto& a : list) {
        const char* dfu = classify_dfu(a.name, a.dfu_service);
        if (sr.dfu_only && dfu == nullptr) continue;
        json::ObjectWriter o;
        const std::string addr = a.mac.str();
        o.add("addr", addr).add("addr_type", a.mac.type ? "random" : "public").add("name", a.name).add("rssi",
                                                                                                       int64_t{a.rssi});
        json::ArrayWriter svcs;
        if (a.dfu_service) svcs.push(kDfuUuid);
        o.add("services", std::move(svcs));
        if (dfu != nullptr) {
            o.add("dfu", dfu);
        } else {
            o.add_null("dfu");
        }
        if (target_exists(t) && matches_target(addr, t.mac)) {
            o.add("matches_target", t.effective_id());
        } else {
            o.add_null("matches_target");
        }
        o.add_bool("connectable", a.connectable);
        arr.push(std::move(o));
    }
    json::ObjectWriter w;
    w.add("devices", std::move(arr))
        .add("hint", "Run `start ota` on the repeater first. MeshCore repeaters don't advertise otherwise.");
    return send_obj(req, "200 OK", w);
}

esp_err_t h_logs(httpd_req_t* req) {
    uint64_t end = 0;
    std::string text;
    const std::string_view since = query_param(req->uri, "since");
    if (!since.empty()) {
        uint64_t from = 0;
        for (const char c : since) {
            if (c < '0' || c > '9' || from > (UINT64_MAX - 9) / 10) {
                return send_error(req, "400 Bad Request", "BAD_REQUEST", "since must be a number");
            }
            from = from * 10 + static_cast<uint64_t>(c - '0');
        }
        text = runlog::idf::log_read(from, kLogChunk, end);
    } else {
        text = runlog::idf::log_read(0, 4096, end);  // no cursor yet: the recent tail
    }
    json::ObjectWriter w;
    w.add("next", static_cast<int64_t>(end)).add("text", text);
    return send_obj(req, "200 OK", w);
}

esp_err_t h_config(httpd_req_t* req) {
    DeviceConfig c;
    if (!config::load_device(c)) return send_error(req, "500 Internal Server Error", "INTERNAL");
    bool reboot = false;
    if (req->method == HTTP_PUT) {
        std::string body;
        esp_err_t body_err = ESP_OK;
        if (!read_body(req, body, body_err)) return body_err;
        const DeviceConfig before = c;
        if (auto e = apply_json(body, c); !e.empty()) return send_error(req, "400 Bad Request", "BAD_REQUEST", e);
        if (!config::save_device(c)) return send_error(req, "500 Internal Server Error", "INTERNAL");
        reboot = c.hostname != before.hostname || c.ntp_server != before.ntp_server;
    }
    json::ObjectWriter w;
    write_json(w, c);
    // Read-only details that /status (unauthenticated) leaves out.
    const net::Status ns = net::status();
    w.add("hostname_active", ns.hostname).add_bool("reboot_required", reboot).add("wifi_ssid", ns.ssid);
    if (const esp_partition_t* running = esp_ota_get_running_partition()) w.add("partition", running->label);
    return send_obj(req, "200 OK", w);
}

void reboot_cb(void*) { esp_restart(); }

/// Reboots shortly, so the response that announces it goes out first.
void schedule_reboot(uint64_t delay_us) {
    static esp_timer_handle_t t = nullptr;
    if (t == nullptr) {
        esp_timer_create_args_t a{};
        a.callback = &reboot_cb;
        a.name = "reboot";
        esp_timer_create(&a, &t);
    }
    esp_timer_stop(t);
    esp_timer_start_once(t, delay_us);
}

esp_err_t h_reboot(httpd_req_t* req) {
    if (job::busy() && query_param(req->uri, "force") != "1") {
        return send_error(req, "409 Conflict", "BUSY", "a job is running (add ?force=1 to reboot anyway)");
    }
    ESP_LOGW(TAG, "reboot requested over HTTP");
    schedule_reboot(500 * 1000);
    json::ObjectWriter w;
    w.add_bool("rebooting", true);
    return send_obj(req, "202 Accepted", w);
}

esp_err_t h_rotate(httpd_req_t* req) {
    const std::string token = issue_token();
    if (token.empty()) return send_error(req, "500 Internal Server Error", "INTERNAL", "could not store the token");
    ESP_LOGI(TAG, "API token rotated");
    json::ObjectWriter w;
    w.add("token", token);
    return send_obj(req, "200 OK", w);
}

esp_err_t h_fault(httpd_req_t* req) {
#if CONFIG_DFU32_DEBUG_ENDPOINTS
    std::string body;
    esp_err_t body_err = ESP_OK;
    if (!read_body(req, body, body_err)) return body_err;
    FaultRequest fr;
    if (auto e = parse_fault_request(body, fr); !e.empty()) return send_error(req, "400 Bad Request", "BAD_REQUEST", e);
    job::Faults f;
    f.restore_fail = fr.restore_fail;
    f.deadline_s = fr.deadline_s;
    f.stall_s = fr.stall_s;
    job::set_faults(f);
    ESP_LOGW(TAG, "fault injection armed for the next flash: restore_fail=%d deadline_s=%" PRIu32 " stall_s=%" PRIu32,
             f.restore_fail, f.deadline_s, f.stall_s);
    json::ObjectWriter w;
    w.add_bool("restore_fail", f.restore_fail).add("deadline_s", int64_t{f.deadline_s}).add("stall_s",
                                                                                             int64_t{f.stall_s});
    return send_obj(req, "200 OK", w);
#else
    return send_error(req, "404 Not Found", "NOT_FOUND", "debug endpoints are not built in (CONFIG_DFU32_DEBUG_ENDPOINTS)");
#endif
}

// ---- firmware upload (POST /api/v1/system/ota) ----------------------------------------

constexpr size_t kOtaChunk = 4096;
constexpr int kOtaMaxTimeouts = 3;  // x recv_wait_timeout (5 s)
constexpr size_t kOtaDrainMax = 4u * 1024 * 1024;  // never read more than this to discard a refused upload

/// Fills `buf` with up to `want` body bytes; false (and `err` set) on a dead client.
bool recv_exact(httpd_req_t* req, uint8_t* buf, size_t want, std::string& err) {
    size_t got = 0;
    int timeouts = 0;
    while (got < want) {
        const int n = httpd_req_recv(req, reinterpret_cast<char*>(buf) + got, want - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < kOtaMaxTimeouts) continue;
        if (n <= 0) {
            err = "upload stalled or the connection closed";
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

/// Reads and discards up to `n` more body bytes (capped), so an early error
/// response isn't answered with a connection reset: browsers that are still
/// sending the upload then see the JSON error instead.
void drain(httpd_req_t* req, size_t n) {
    char buf[512];
    if (n > kOtaDrainMax) n = kOtaDrainMax;
    int timeouts = 0;
    while (n > 0) {
        const int got = httpd_req_recv(req, buf, n < sizeof(buf) ? n : sizeof(buf));
        if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < kOtaMaxTimeouts) continue;
        if (got <= 0) break;
        n -= static_cast<size_t>(got);
    }
}

/// Streams the body into the other OTA slot. The image is checked (chip,
/// project, board tag) before anything is written, verified by esp_ota_end(),
/// and only then made the boot partition. Rollback: the new image must come
/// up with WiFi and HTTP, or the bootloader returns to this one (main.cpp).
esp_err_t h_ota(httpd_req_t* req, bool force) {
    if (req->content_len == 0) {
        return send_error(req, "411 Length Required", "BAD_REQUEST", "send dfu32.bin as the raw body, with Content-Length");
    }
    size_t remaining = req->content_len;
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (next == nullptr) {
        drain(req, remaining);
        return send_error(req, "500 Internal Server Error", "INTERNAL", "no OTA partition to update");
    }
    if (req->content_len > next->size) {
        drain(req, remaining);
        return send_error(req, "413 Content Too Large", "BAD_REQUEST",
                          "image is " + std::to_string(req->content_len) + " B, the OTA slot holds " +
                              std::to_string(next->size) + " B");
    }
    job::Lease lease("ota");
    if (!lease.ok()) {
        drain(req, remaining);
        return send_error(req, "409 Conflict", "BUSY", busy_detail());
    }

    std::vector<uint8_t> buf(kOtaChunk);
    std::string err;
    ota::ImageInfo info;
    esp_ota_handle_t h = 0;
    bool begun = false;
    // `remaining` is what the client has yet to send; read it (bounded) before
    // answering, except when the client itself stopped sending.
    bool client_gone = false;
    const auto fail = [&](const char* status, const char* code, const std::string& detail) {
        if (begun) esp_ota_abort(h);
        ESP_LOGE(TAG, "firmware upload refused: %s", detail.c_str());
        if (!client_gone) drain(req, remaining);
        return send_error(req, status, code, detail);
    };
    const int64_t t0 = esp_timer_get_time();
    unsigned last_decile = 0;
    while (remaining > 0) {
        const size_t want = remaining < buf.size() ? remaining : buf.size();
        if (!recv_exact(req, buf.data(), want, err)) {
            client_gone = true;
            return fail("408 Request Timeout", "BAD_REQUEST", err);
        }
        remaining -= want;  // consumed from the socket, whatever happens next
        if (!begun) {
            ota::Expect expect;
            expect.chip_id = static_cast<uint16_t>(CONFIG_IDF_FIRMWARE_CHIP_ID);
            expect.project = esp_app_get_description()->project_name;
            expect.board = std::string_view(dfu32_board_tag.board, strnlen(dfu32_board_tag.board, sizeof(dfu32_board_tag.board)));
            expect.allow_other_board = force;
            if (auto why = ota::check_image_start({buf.data(), want}, expect, info); !why.empty()) {
                return fail("422 Unprocessable Content", "IMAGE_REJECTED", why);
            }
            const esp_err_t e = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &h);
            if (e != ESP_OK) return fail("500 Internal Server Error", "STAGING_IO", std::string("esp_ota_begin: ") + esp_err_to_name(e));
            begun = true;
            ESP_LOGI(TAG, "firmware upload: \"%s\" (%s) into %s, %u B", info.version.c_str(),
                     info.board.empty() ? "untagged" : info.board.c_str(), next->label,
                     static_cast<unsigned>(req->content_len));
        }
        if (const esp_err_t e = esp_ota_write(h, buf.data(), want); e != ESP_OK) {
            return fail("500 Internal Server Error", "STAGING_IO", std::string("esp_ota_write: ") + esp_err_to_name(e));
        }
        const unsigned decile = static_cast<unsigned>((req->content_len - remaining) * 10 / req->content_len);
        if (decile > last_decile && decile < 10) {
            last_decile = decile;
            ESP_LOGI(TAG, "firmware upload %u%%", decile * 10);
        }
    }
    begun = false;  // esp_ota_end frees the handle whatever it returns
    if (const esp_err_t e = esp_ota_end(h); e != ESP_OK) {
        const bool invalid = e == ESP_ERR_OTA_VALIDATE_FAILED || e == ESP_ERR_INVALID_SIZE;
        return fail(invalid ? "422 Unprocessable Content" : "500 Internal Server Error",
                    invalid ? "IMAGE_REJECTED" : "STAGING_IO",
                    std::string("image failed verification: ") + esp_err_to_name(e));
    }
    if (const esp_err_t e = esp_ota_set_boot_partition(next); e != ESP_OK) {
        return fail("500 Internal Server Error", "STAGING_IO", std::string("esp_ota_set_boot_partition: ") + esp_err_to_name(e));
    }
    const auto ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
    ESP_LOGW(TAG, "firmware \"%s\" written to %s in %u ms; rebooting into it", info.version.c_str(), next->label,
             static_cast<unsigned>(ms));
    json::ObjectWriter w;
    w.add("version", info.version).add("partition", next->label).add_bool("rebooting", true).add(
        "note", "If the new firmware can't bring up WiFi and the API, it rolls back to the current one on its next reboot.");
    // Keep the BLE lease until the reboot, so no job starts in the last second
    // and gets cut off by the restart.
    lease.keep();
    const esp_err_t sent = send_obj(req, "200 OK", w);
    schedule_reboot(1000 * 1000);
    return sent;
}

// ---- image upload into staging (POST /api/v1/staging) ------------------------------

/// The staged header as GET /staging reports it (without verify).
void write_staged_header(json::ObjectWriter& w, const dfu::StagingHeader& h) {
    w.add("target", h.target_id)
        .add("board", h.board)
        .add("version", h.version)
        .add("bin_size", int64_t{h.bin_size})
        .add("sha256", sha256_hex(h.bin_sha256))
        .add("dat_size", int64_t{h.dat_size})
        .add("staged_at", static_cast<int64_t>(h.staged_at))
        .add("manifest_url", h.manifest_url)
        .add("manifest_sha256", sha256_hex(h.manifest_sha256));
}

/// Streams the body (the application .bin) into the staging partition, with
/// the .dat, version and optional board from the query (SPEC.md §8, §10.1).
/// Nothing is erased until the query, the .dat and the size pass their
/// checks; from then on a failure leaves nothing staged. The .dat's CRC16
/// over what arrived is the end-to-end check, as when staging over USB.
esp_err_t h_stage_upload(httpd_req_t* req) {
    size_t remaining = req->content_len;
    StagingUpload q;
    if (auto e = parse_staging_upload(req->uri, q); !e.empty()) {
        drain(req, remaining);
        return send_error(req, "400 Bad Request", "BAD_REQUEST", e);
    }
    if (req->content_len == 0) {
        return send_error(req, "411 Length Required", "BAD_REQUEST",
                          "send the application .bin as the raw body, with Content-Length");
    }
    const esp_partition_t* part = PartitionRegion::find_staging();
    if (part == nullptr) {
        drain(req, remaining);
        return send_error(req, "500 Internal Server Error", "STAGING_IO", "no staging partition");
    }
    job::Lease lease("upload");
    if (!lease.ok()) {
        drain(req, remaining);
        return send_error(req, "409 Conflict", "BUSY", busy_detail());
    }

    // The flash re-checks the size against the target's limit, so use the
    // same one here when it is set.
    TargetConfig t;
    StageMeta meta;
    if (config::load_target(t) && target_exists(t)) meta.max_app_size = t.max_app_size;
    meta.board = q.board;
    meta.manifest_url = q.source;
    meta.staged_at = net::unix_time();
    UploadInfo up;
    up.bin_size = static_cast<uint32_t>(req->content_len);
    up.version = q.version;
    up.has_sha256 = q.has_sha256;
    up.bin_sha256 = q.sha256;

    PartitionRegion region(part);
    PsaSha256 sha;
    StagingWriter w(region, sha);
    bool client_gone = false;
    const auto fail = [&](const Status& st) {
        ESP_LOGE(TAG, "image upload refused: %s", st.str().c_str());
        if (!client_gone) drain(req, remaining);
        std::string detail = st.detail;
        if (w.invalidated_previous()) detail += " (the previously staged image was invalidated)";
        const char* http = st.code == Err::StagingIo ? "500 Internal Server Error" : "422 Unprocessable Content";
        return send_error(req, http, to_string(st.code), detail);
    };
    if (Status st = w.begin_upload(q.dat, up, meta); !st.ok()) return fail(st);
    ESP_LOGI(TAG, "image upload: \"%s\" (%s), %u B", q.version.c_str(), q.source.c_str(),
             static_cast<unsigned>(req->content_len));

    std::vector<uint8_t> buf(kOtaChunk);
    std::string err;
    const int64_t t0 = esp_timer_get_time();
    unsigned last_decile = 0;
    while (remaining > 0) {
        const size_t want = remaining < buf.size() ? remaining : buf.size();
        if (!recv_exact(req, buf.data(), want, err)) {
            client_gone = true;
            ESP_LOGE(TAG, "image upload: %s", err.c_str());
            std::string detail = err;
            if (w.invalidated_previous()) detail += " (the previously staged image was invalidated)";
            return send_error(req, "408 Request Timeout", "BAD_REQUEST", detail);
        }
        remaining -= want;
        if (Status st = w.write({buf.data(), want}); !st.ok()) return fail(st);
        const unsigned decile = static_cast<unsigned>((req->content_len - remaining) * 10 / req->content_len);
        if (decile > last_decile && decile < 10) {
            last_decile = decile;
            ESP_LOGI(TAG, "image upload %u%%", decile * 10);
        }
    }
    if (Status st = w.finish(); !st.ok()) return fail(st);
    ESP_LOGI(TAG, "staged \"%s\" from %s: %u B in %u ms", q.version.c_str(), q.source.c_str(),
             static_cast<unsigned>(req->content_len), static_cast<unsigned>((esp_timer_get_time() - t0) / 1000));
    json::ObjectWriter o;
    o.add_bool("valid", true);
    write_staged_header(o, w.header());
    return send_obj(req, "200 OK", o);
}

// ---- slow requests (check, scan, firmware upload) on their own task ---------------
// esp_http_server has one worker thread. A manifest fetch or a BLE scan can
// take many seconds, so those requests are handed to this task through
// httpd's async-request API and the server keeps answering /status and
// /jobs/current/abort meanwhile. One slow request at a time.

struct SlowRequest {
    httpd_req_t* req = nullptr;  // async copy; must be completed
    Route route = Route::NotFound;
    RouteMatch match;
    ScanRequest scan;
    bool force = false;  // SystemOta: ?force=1
};

QueueHandle_t g_slow_q = nullptr;
std::atomic<bool> g_slow_busy{false};

void slow_task(void*) {
    for (;;) {
        SlowRequest* raw = nullptr;
        if (xQueueReceive(g_slow_q, &raw, portMAX_DELAY) != pdTRUE || raw == nullptr) continue;
        std::unique_ptr<SlowRequest> r(raw);
        if (r->route == Route::TargetCheck) {
            h_check(r->req, r->match);
        } else if (r->route == Route::SystemOta) {
            h_ota(r->req, r->force);
        } else if (r->route == Route::Staging) {
            h_stage_upload(r->req);
        } else {
            h_scan(r->req, r->scan);
        }
        httpd_req_async_handler_complete(r->req);
        g_slow_busy = false;
    }
}

esp_err_t start_slow(httpd_req_t* req, std::unique_ptr<SlowRequest> r) {
    if (g_slow_q == nullptr) return send_error(req, "500 Internal Server Error", "INTERNAL", "worker not running");
    if (g_slow_busy.exchange(true)) {
        return send_error(req, "409 Conflict", "BUSY", "another check, scan or upload is still running");
    }
    if (httpd_req_async_handler_begin(req, &r->req) != ESP_OK) {
        g_slow_busy = false;
        return send_error(req, "500 Internal Server Error", "INTERNAL", "cannot start async request");
    }
    SlowRequest* raw = r.release();
    if (xQueueSend(g_slow_q, &raw, 0) != pdTRUE) {  // can't happen: one at a time
        httpd_req_t* copy = raw->req;
        delete raw;
        send_error(copy, "500 Internal Server Error", "INTERNAL", "worker queue full");
        httpd_req_async_handler_complete(copy);
        g_slow_busy = false;
    }
    return ESP_OK;
}

esp_err_t queue_check(httpd_req_t* req, const RouteMatch& m) {
    auto r = std::make_unique<SlowRequest>();
    r->route = Route::TargetCheck;
    r->match = m;
    return start_slow(req, std::move(r));
}

esp_err_t queue_ota(httpd_req_t* req) {
    if (httpd_req_get_hdr_value_len(req, "Transfer-Encoding") != 0) {
        return send_error(req, "411 Length Required", "BAD_REQUEST", "send the image with Content-Length, not chunked");
    }
    auto r = std::make_unique<SlowRequest>();
    r->route = Route::SystemOta;
    r->force = query_param(req->uri, "force") == "1";
    return start_slow(req, std::move(r));  // the body is read on the worker
}

esp_err_t queue_stage_upload(httpd_req_t* req) {
    if (httpd_req_get_hdr_value_len(req, "Transfer-Encoding") != 0) {
        return send_error(req, "411 Length Required", "BAD_REQUEST", "send the .bin with Content-Length, not chunked");
    }
    auto r = std::make_unique<SlowRequest>();
    r->route = Route::Staging;
    return start_slow(req, std::move(r));  // the body is read on the worker
}

esp_err_t queue_scan(httpd_req_t* req) {
    std::string body;
    esp_err_t body_err = ESP_OK;
    if (!read_body(req, body, body_err)) return body_err;
    auto r = std::make_unique<SlowRequest>();
    r->route = Route::BleScan;
    if (auto e = parse_scan_request(body, r->scan); !e.empty()) {
        return send_error(req, "400 Bad Request", "BAD_REQUEST", e);
    }
    return start_slow(req, std::move(r));
}

Method method_of(httpd_req_t* req) {
    switch (req->method) {
        case HTTP_GET: return Method::Get;
        case HTTP_PUT: return Method::Put;
        case HTTP_POST: return Method::Post;
        case HTTP_DELETE: return Method::Delete;
        default: return Method::Other;
    }
}

esp_err_t dispatch(httpd_req_t* req) {
    const RouteMatch m = match_route(req->uri);
    if (m.route == Route::NotFound) return send_error(req, "404 Not Found", "NOT_FOUND", "no such endpoint");
    if (!method_allowed(m.route, method_of(req))) {
        return send_error(req, "405 Method Not Allowed", "METHOD_NOT_ALLOWED");
    }
    if (!is_public(m.route)) {
        esp_err_t sent = ESP_OK;
        if (!authorize(req, sent)) return sent;
    }
    switch (m.route) {
        case Route::Status: return h_status(req);
        case Route::Claim: return h_claim(req);
        case Route::Targets: return h_targets(req);
        case Route::Target: return h_target(req, m);
        case Route::TargetCheck: return queue_check(req, m);
        case Route::TargetStage: return h_stage(req, m);
        case Route::TargetFlash: return h_flash(req, m);
        case Route::TargetRuns: return h_target_runs(req, m);
        case Route::JobAbort: return h_abort(req);
        case Route::Run: return h_run(req, m);
        case Route::Staging: return req->method == HTTP_POST ? queue_stage_upload(req) : h_staging(req);
        case Route::BleScan: return queue_scan(req);
        case Route::SystemOta: return queue_ota(req);
        case Route::Logs: return h_logs(req);
        case Route::Config: return h_config(req);
        case Route::Reboot: return h_reboot(req);
        case Route::TokenRotate: return h_rotate(req);
        case Route::DebugFault: return h_fault(req);
        case Route::NotFound: break;
    }
    return send_error(req, "404 Not Found", "NOT_FOUND");
}

// The web UI (SPEC.md §10.2): components/api/ui/index.html, gzipped at build
// time (tools/dfu32_ui.py) and embedded. It works while unclaimed (the claim
// screen), which is where an Improv redirect lands.
extern "C" const uint8_t ui_gz_start[] asm("_binary_index_html_gz_start");
extern "C" const uint8_t ui_gz_end[] asm("_binary_index_html_gz_end");

esp_err_t h_root(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, reinterpret_cast<const char*>(ui_gz_start), ui_gz_end - ui_gz_start);
}

}  // namespace

std::string rotate_token() { return issue_token(); }

bool unclaim() {
    if (!config::clear_token()) return false;
    std::lock_guard<std::mutex> lk(g_auth_mu);
    g_claimed = false;
    g_token_hash = {};
    return true;
}

bool start() {
    if (g_server != nullptr) return true;
    load_token();
    g_slow_q = xQueueCreate(1, sizeof(SlowRequest*));
    if (g_slow_q == nullptr || xTaskCreate(&slow_task, "api_slow", kSlowTaskStack, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "api worker task create failed");
        return false;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;  // JSON building; the slow requests run on api_slow
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;
    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        g_server = nullptr;
        return false;
    }
    const httpd_method_t methods[] = {HTTP_GET, HTTP_PUT, HTTP_POST, HTTP_DELETE};
    for (const httpd_method_t m : methods) {
        httpd_uri_t u{};
        u.uri = "/api/v1/*";
        u.method = m;
        u.handler = &dispatch;
        httpd_register_uri_handler(g_server, &u);
    }
    httpd_uri_t root{};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = &h_root;
    httpd_register_uri_handler(g_server, &root);
    net::advertise_http(static_cast<uint16_t>(cfg.server_port));
    ESP_LOGI(TAG, "HTTP API on port %u (%s)", cfg.server_port, claimed() ? "claimed" : "unclaimed: POST /api/v1/claim");
    return true;
}

}  // namespace dfu32::api
