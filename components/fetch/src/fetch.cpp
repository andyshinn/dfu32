// SPDX-License-Identifier: MIT
#include "fetch/fetch.hpp"

#include <cinttypes>
#include <cstdio>
#include <vector>

#include "dfu32_core/stage_writer.hpp"
#include "dfu32_core/url.hpp"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "staging/staging.hpp"
#include "staging/staging_io.hpp"

namespace dfu32::fetch {
namespace {

constexpr const char* TAG = "fetch";
constexpr int kTimeoutMs = 15000;
constexpr int kMaxRedirects = 3;
constexpr size_t kReadChunk = 4096;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

Status http_error(std::string detail) { return Status::fail(Err::HttpError, std::move(detail)); }

const char* user_agent() {
    static char ua[48] = {};
    if (ua[0] == 0) std::snprintf(ua, sizeof(ua), "DFU32/%s", esp_app_get_description()->version);
    return ua;
}

class Client {
public:
    ~Client() {
        if (h_ != nullptr) {
            esp_http_client_close(h_);
            esp_http_client_cleanup(h_);
        }
    }

    // Connects, follows up to kMaxRedirects redirects, and leaves the client
    // positioned at the start of a 200 response body.
    Status open(const std::string& url) {
        esp_http_client_config_t cfg{};
        cfg.url = url.c_str();
        cfg.timeout_ms = kTimeoutMs;
        cfg.buffer_size = 2048;
        cfg.user_agent = user_agent();
        cfg.disable_auto_redirect = true;  // handled below, so open/read mode sees every hop
        h_ = esp_http_client_init(&cfg);
        if (h_ == nullptr) return http_error("bad URL " + url);
        for (int hop = 0;; ++hop) {
            const esp_err_t err = esp_http_client_open(h_, 0);
            if (err != ESP_OK) return http_error(std::string("connect failed (") + esp_err_to_name(err) + ") for " + url);
            length_ = esp_http_client_fetch_headers(h_);
            if (length_ < 0) return http_error("no response headers from " + url);
            const int status = esp_http_client_get_status_code(h_);
            const bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
            if (redirect && hop < kMaxRedirects) {
                int flushed = 0;
                esp_http_client_flush_response(h_, &flushed);
                if (esp_http_client_set_redirection(h_) != ESP_OK) {
                    return http_error("HTTP " + std::to_string(status) + " without a usable Location from " + url);
                }
                esp_http_client_close(h_);
                // The client is built without TLS: refuse https (and anything
                // else) with the same message a configured URL would get.
                const std::string next = current_url();
                Url parsed;
                std::string why;
                if (!parse_http_url(next, parsed, &why)) return http_error("redirect to " + next + ": " + why);
                ESP_LOGI(TAG, "following HTTP %d redirect to %s", status, next.c_str());
                continue;
            }
            if (status != 200) return http_error("HTTP " + std::to_string(status) + " for " + url);
            chunked_ = esp_http_client_is_chunked_response(h_);
            final_url_ = current_url();
            if (final_url_.empty()) final_url_ = url;
            return {};
        }
    }

    /// The URL that answered 200, after any redirects.
    const std::string& final_url() const { return final_url_; }

    /// Content-Length, or -1 when the server didn't send one (chunked).
    int64_t length() const { return chunked_ ? -1 : length_; }

    /// Bytes read (> 0), 0 at a clean end of body, or an HTTP_ERROR.
    Status read(std::span<uint8_t> buf, size_t& got, uint32_t so_far) {
        got = 0;
        const int n = esp_http_client_read(h_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()));
        if (n < 0) return http_error("read failed after " + std::to_string(so_far) + " B");
        if (n == 0 && !esp_http_client_is_complete_data_received(h_)) {
            return http_error("connection closed after " + std::to_string(so_far) + " B");
        }
        got = static_cast<size_t>(n);
        return {};
    }

private:
    std::string current_url() const {
        char buf[kMaxUrlLen + 64] = {};
        return esp_http_client_get_url(h_, buf, sizeof(buf)) == ESP_OK ? std::string(buf) : std::string();
    }

    esp_http_client_handle_t h_ = nullptr;
    std::string final_url_;
    int64_t length_ = 0;
    bool chunked_ = false;
};

bool sha256_of(std::string_view text, Sha256& out) {
    PsaSha256 sha;
    return sha.reset() && sha.update({reinterpret_cast<const uint8_t*>(text.data()), text.size()}) && sha.finish(out);
}

Status get_text_impl(const std::string& url, size_t max_bytes, std::string& out, std::string* final_url);

// Relative file URLs resolve against where the manifest was actually served
// from (after redirects), so a "/latest" redirect to "/v1.2/manifest.json"
// finds "/v1.2/firmware.bin".
Status fetch_manifest(const std::string& url, Manifest& m, Sha256& digest) {
    if (url.empty()) return Status::fail(Err::ManifestInvalid, "no manifest URL set for the target");
    std::string text;
    std::string served_from;
    ESP_LOGI(TAG, "fetching manifest %s", url.c_str());
    if (Status s = get_text_impl(url, kMaxManifestBytes, text, &served_from); !s.ok()) {
        if (s.code == Err::SizeMismatch) return Status::fail(Err::ManifestInvalid, "larger than 8 KB");
        return s;
    }
    if (!sha256_of(text, digest)) return Status::fail(Err::StagingIo, "SHA-256 failed");
    return parse_manifest(text, served_from, m);
}

Status get_text_impl(const std::string& url, size_t max_bytes, std::string& out, std::string* final_url) {
    out.clear();
    Client c;
    if (Status s = c.open(url); !s.ok()) return s;
    if (final_url != nullptr) *final_url = c.final_url();
    if (c.length() > static_cast<int64_t>(max_bytes)) {
        return Status::fail(Err::SizeMismatch, "response is " + std::to_string(c.length()) + " B, limit " +
                                                   std::to_string(max_bytes) + " B");
    }
    uint8_t buf[512];
    for (;;) {
        size_t got = 0;
        if (Status s = c.read(buf, got, static_cast<uint32_t>(out.size())); !s.ok()) return s;
        if (got == 0) return {};
        out.append(reinterpret_cast<const char*>(buf), got);
        if (out.size() > max_bytes) {
            return Status::fail(Err::SizeMismatch, "response exceeds " + std::to_string(max_bytes) + " B");
        }
    }
}

}  // namespace

Status get_text(const std::string& url, size_t max_bytes, std::string& out) {
    return get_text_impl(url, max_bytes, out, nullptr);
}

Status get_stream(const std::string& url, uint32_t expected_size, const std::function<Status()>& on_headers,
                  const std::function<Status(std::span<const uint8_t>)>& sink, const ProgressFn& progress) {
    Client c;
    if (Status s = c.open(url); !s.ok()) return s;
    if (c.length() >= 0 && c.length() != expected_size) {
        return Status::fail(Err::SizeMismatch, "server sends " + std::to_string(c.length()) + " B, manifest says " +
                                                   std::to_string(expected_size) + " B");
    }
    if (on_headers) {
        if (Status s = on_headers(); !s.ok()) return s;
    }
    std::vector<uint8_t> buf(kReadChunk);
    uint32_t total = 0;
    unsigned last_decile = 0;
    for (;;) {
        size_t got = 0;
        if (Status s = c.read(buf, got, total); !s.ok()) return s;
        if (got == 0) return {};
        if (Status s = sink({buf.data(), got}); !s.ok()) return s;
        total += static_cast<uint32_t>(got);
        if (progress) progress(total, expected_size);
        const unsigned decile = expected_size ? static_cast<unsigned>(uint64_t{total} * 10 / expected_size) : 0;
        if (decile > last_decile && decile < 10) {
            last_decile = decile;
            ESP_LOGI(TAG, "%u%% (%" PRIu32 " / %" PRIu32 " B)", decile * 10, total, expected_size);
        }
    }
}

Status check(const std::string& manifest_url, const std::string& board, CheckReport& out) {
    out = CheckReport{};
    if (Status s = fetch_manifest(manifest_url, out.manifest, out.manifest_sha256); !s.ok()) return s;
    out.board_ok = board_matches(out.manifest, board);
    StagedImage img;
    if (img.open() && img.header_status() == dfu::StagingHeaderError::Ok) {
        out.staged_valid = true;
        out.staged = img.header();
        out.staged_same = same_image(out.manifest, out.staged);
    }
    return {};
}

Status stage(const TargetConfig& target, const std::string& manifest_url, const StageOptions& opts,
             StageReport& out) {
    out = StageReport{};
    const uint32_t t0 = now_ms();
    const std::string url = manifest_url.empty() ? target.manifest_url : manifest_url;
    Sha256 manifest_sha{};
    if (Status s = fetch_manifest(url, out.manifest, manifest_sha); !s.ok()) return s;
    const Manifest& m = out.manifest;

    const esp_partition_t* part = PartitionRegion::find_staging();
    if (part == nullptr) return Status::fail(Err::StagingIo, "no staging partition");
    PartitionRegion region(part);
    PsaSha256 sha;

    StageMeta meta;
    meta.target_id = target.effective_id();
    meta.board = target.board;
    meta.manifest_url = url;
    meta.manifest_sha256 = manifest_sha;
    meta.staged_at = opts.staged_at;
    meta.max_app_size = target.max_app_size;

    // Already staged? Re-check it against this manifest and target, re-hash, done.
    StagedImage img;
    if (!opts.force && img.open() && img.header_status() == dfu::StagingHeaderError::Ok &&
        same_image(m, img.header())) {
        if (Status s = precheck_image(m, img.header().dat_bytes(), meta, region.size(), sha, out.init); !s.ok()) {
            return s;
        }
        const StagingVerify v = img.verify(target.max_app_size);
        if (v.ok) {
            bool rewritten = false;
            const Status hs = refresh_staged_header(region, img.header(), m, meta, rewritten);
            out.invalidated = rewritten && !hs.ok();
            if (!hs.ok()) return hs;
            if (rewritten) ESP_LOGI(TAG, "staged header updated for target '%s'", meta.target_id.c_str());
            out.reused = true;
            out.ms = now_ms() - t0;
            ESP_LOGI(TAG, "\"%s\" is already staged and verifies; not downloading again", m.version.c_str());
            return {};
        }
        ESP_LOGW(TAG, "staged copy of \"%s\" failed re-verification (%s); downloading again", m.version.c_str(),
                 v.error.c_str());
    }

    // .dat first (tiny), so every precheck runs before anything is erased.
    std::string dat_text;
    ESP_LOGI(TAG, "fetching .dat %s", m.dat.url.c_str());
    if (Status s = get_text(m.dat.url, dfu::kStagingMaxDat, dat_text); !s.ok()) {
        if (s.code == Err::SizeMismatch) return Status::fail(Err::DatTooLong, s.detail);
        return s;
    }
    const std::span<const uint8_t> dat(reinterpret_cast<const uint8_t*>(dat_text.data()), dat_text.size());
    if (Status s = precheck_image(m, dat, meta, region.size(), sha, out.init); !s.ok()) return s;

    StagingWriter writer(region, sha);
    ESP_LOGI(TAG, "staging \"%s\" (%" PRIu32 " B) from %s", m.version.c_str(), m.bin.size, m.bin.url.c_str());
    const uint32_t t_bin = now_ms();
    Status s = get_stream(
        m.bin.url, m.bin.size, [&] { return writer.begin(m, dat, meta); },
        [&](std::span<const uint8_t> chunk) { return writer.write(chunk); }, opts.progress);
    if (s.ok()) s = writer.finish();
    out.bytes = writer.written();
    out.invalidated = !s.ok() && writer.invalidated_previous();
    out.ms = now_ms() - t0;
    if (!s.ok()) {
        ESP_LOGE(TAG, "staging failed: %s", s.str().c_str());
        return s;
    }
    const uint32_t bin_ms = now_ms() - t_bin;
    ESP_LOGI(TAG, "staged \"%s\": %" PRIu32 " B in %" PRIu32 " ms (%" PRIu32 " kB/s incl. verify)", m.version.c_str(),
             out.bytes, bin_ms, bin_ms ? out.bytes / bin_ms : 0);
    return {};
}

}  // namespace dfu32::fetch
