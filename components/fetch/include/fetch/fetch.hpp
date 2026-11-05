// SPDX-License-Identifier: MIT
//
// HTTP fetch of a target's manifest and image into the staging partition
// (SPEC.md §3.1 steps 2-3, §7.2, §8). Plain HTTP only (v1).
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "dfu32_core/errors.hpp"
#include "dfu32_core/manifest.hpp"
#include "dfu32_core/target_config.hpp"
#include "dfu_proto/init_packet.hpp"
#include "dfu_proto/staging_format.hpp"

namespace dfu32::fetch {

using ProgressFn = std::function<void(uint32_t done, uint32_t total)>;

/// GETs `url` into `out`. HTTP_ERROR for connection/status problems,
/// SIZE_MISMATCH if the body exceeds `max_bytes`.
Status get_text(const std::string& url, size_t max_bytes, std::string& out);

/// GETs `url` and feeds the body to `sink` in chunks. `on_headers` runs once a
/// 200 response with an acceptable length has arrived and before any body is
/// read (so nothing is erased on a 404). If the server sends Content-Length,
/// it must equal `expected_size` (SIZE_MISMATCH otherwise).
Status get_stream(const std::string& url, uint32_t expected_size, const std::function<Status()>& on_headers,
                  const std::function<Status(std::span<const uint8_t>)>& sink, const ProgressFn& progress = {});

struct CheckReport {
    Manifest manifest;
    Sha256 manifest_sha256{};
    bool board_ok = false;
    bool staged_valid = false;       // the staging header decodes (not re-hashed here)
    bool staged_same = false;        // staged image has the manifest's .bin and .dat hashes
    dfu::StagingHeader staged{};
};

/// Fetches and parses the manifest; compares it with the target's board and
/// with what's staged. Writes nothing (POST /targets/{id}/check).
Status check(const std::string& manifest_url, const std::string& board, CheckReport& out);

struct StageOptions {
    bool force = false;        // download even if the staged image already matches
    uint64_t staged_at = 0;    // unix seconds for the header (0 = unknown)
    ProgressFn progress;
};

struct StageReport {
    Manifest manifest;
    bool reused = false;       // the staged image already matched and re-verified OK
    uint32_t bytes = 0;        // .bin bytes downloaded
    uint32_t ms = 0;           // total time
    dfu::InitPacket init{};
};

/// Fetch, verify and stage `target`'s image from `manifest_url` (or the
/// target's own URL when empty). On a precheck failure the previously staged
/// image is untouched; once the download starts it is invalidated.
Status stage(const TargetConfig& target, const std::string& manifest_url, const StageOptions& opts,
             StageReport& out);

}  // namespace dfu32::fetch
