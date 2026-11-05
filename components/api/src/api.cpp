// SPDX-License-Identifier: MIT
#include "api/api.hpp"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "dfu32_core/api_core.hpp"
#include "dfu32_core/job.hpp"
#include "dfu32_core/json.hpp"
#include "dfu32_core/manifest.hpp"
#include "dfu_ble/dfu_ble.hpp"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fetch/fetch.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "job/job.hpp"
#include "net/net.hpp"
#include "psa/crypto.h"
#include "runlog/run_record.hpp"
#include "runlog/runlog_idf.hpp"
#include "sdkconfig.h"
#include "staging/staging.hpp"

namespace dfu32::api {
namespace {

constexpr const char* TAG = "api";
constexpr size_t kMaxBody = 4096;
constexpr size_t kLogChunk = 8192;
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

bool read_body(httpd_req_t* req, std::string& out) {
    out.clear();
    if (req->content_len > kMaxBody) return false;
    out.resize(req->content_len);
    size_t got = 0;
    while (got < out.size()) {
        const int n = httpd_req_recv(req, out.data() + got, out.size() - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

std::string header(httpd_req_t* req, const char* name) {
    const size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0) return {};
    std::string v(len + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, name, v.data(), v.size()) != ESP_OK) return {};
    v.resize(len);
    return v;
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
    uint32_t delay = 0;
    {
        std::lock_guard<std::mutex> lk(g_auth_mu);
        delay = g_throttle.delay_ms(now_ms());
    }
    if (delay) vTaskDelay(pdMS_TO_TICKS(delay));
    std::array<uint8_t, 32> want{};
    {
        std::lock_guard<std::mutex> lk(g_auth_mu);
        if (!g_claimed) {
            sent = send_error(req, "403 Forbidden", "UNCLAIMED", "claim the device first: POST /api/v1/claim");
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

    if (job::busy()) return send_error(req, "409 Conflict", "BUSY", "a job is running");
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
    if (!read_body(req, body)) return send_error(req, "413 Content Too Large", "BAD_REQUEST", "body too large");
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
    fetch::CheckReport r;
    const Status s = fetch::check(t.manifest_url, t.board, r);
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
    if (!read_body(req, body)) return send_error(req, "413 Content Too Large", "BAD_REQUEST", "body too large");
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
    if (!read_body(req, body)) return send_error(req, "413 Content Too Large", "BAD_REQUEST", "body too large");
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
        if (!lease.ok()) return send_error(req, "409 Conflict", "BUSY", "a job is running");
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
            if (!lease.ok()) return send_error(req, "409 Conflict", "BUSY", "a job is running");
            const StagingVerify v = img.verify();
            w.add_bool("verified", v.ok).add("verify_error", v.error);
        }
    }
    return send_obj(req, "200 OK", w);
}

esp_err_t h_scan(httpd_req_t* req) {
    std::string body;
    if (!read_body(req, body)) return send_error(req, "413 Content Too Large", "BAD_REQUEST", "body too large");
    ScanRequest sr;
    if (auto e = parse_scan_request(body, sr); !e.empty()) return send_error(req, "400 Bad Request", "BAD_REQUEST", e);
    TargetConfig t;
    config::load_target(t);
    std::vector<ble::Advert> list;
    {
        job::Lease lease("scan");
        if (!lease.ok()) return send_error(req, "409 Conflict", "BUSY", "a job is running");
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
        if (!read_body(req, body)) return send_error(req, "413 Content Too Large", "BAD_REQUEST", "body too large");
        const DeviceConfig before = c;
        if (auto e = apply_json(body, c); !e.empty()) return send_error(req, "400 Bad Request", "BAD_REQUEST", e);
        if (!config::save_device(c)) return send_error(req, "500 Internal Server Error", "INTERNAL");
        reboot = c.hostname != before.hostname || c.ntp_server != before.ntp_server;
    }
    json::ObjectWriter w;
    write_json(w, c);
    w.add("hostname_active", net::status().hostname).add_bool("reboot_required", reboot);
    return send_obj(req, "200 OK", w);
}

void reboot_cb(void*) { esp_restart(); }

esp_err_t h_reboot(httpd_req_t* req) {
    if (job::busy() && query_param(req->uri, "force") != "1") {
        return send_error(req, "409 Conflict", "BUSY", "a job is running (add ?force=1 to reboot anyway)");
    }
    static esp_timer_handle_t t = nullptr;
    if (t == nullptr) {
        esp_timer_create_args_t a{};
        a.callback = &reboot_cb;
        a.name = "reboot";
        esp_timer_create(&a, &t);
    }
    ESP_LOGW(TAG, "reboot requested over HTTP");
    esp_timer_start_once(t, 500 * 1000);  // let the response go out first
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
    if (!read_body(req, body)) return send_error(req, "413 Content Too Large", "BAD_REQUEST", "body too large");
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
        case Route::TargetCheck: return h_check(req, m);
        case Route::TargetStage: return h_stage(req, m);
        case Route::TargetFlash: return h_flash(req, m);
        case Route::TargetRuns: return h_target_runs(req, m);
        case Route::JobAbort: return h_abort(req);
        case Route::Run: return h_run(req, m);
        case Route::Staging: return h_staging(req);
        case Route::BleScan: return h_scan(req);
        case Route::Logs: return h_logs(req);
        case Route::Config: return h_config(req);
        case Route::Reboot: return h_reboot(req);
        case Route::TokenRotate: return h_rotate(req);
        case Route::DebugFault: return h_fault(req);
        case Route::NotFound: break;
    }
    return send_error(req, "404 Not Found", "NOT_FOUND");
}

esp_err_t h_root(httpd_req_t* req) {
    static constexpr char kPage[] =
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
        "<title>DFU32</title><body style='font-family:system-ui;max-width:40em;margin:2em auto;padding:0 1em'>"
        "<h1>DFU32</h1><p>The web UI arrives in M4. Until then use the JSON API: "
        "<a href='/api/v1/status'>/api/v1/status</a>. See docs/BENCH.md for curl examples.</p>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kPage, sizeof(kPage) - 1);
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
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 12288;  // check runs the HTTP client and manifest parser on this task
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
