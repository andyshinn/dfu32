// SPDX-License-Identifier: MIT
//
// Per-target image manifest (SPEC.md §11):
//
//   {"schema": 1, "version": "v1.18.0-81dbae5", "boards": ["rak4631"],
//    "bin": {"url": "firmware.bin", "size": 786432, "sha256": "<64 hex>"},
//    "dat": {"url": "firmware.dat", "size": 14, "sha256": "<64 hex>"},
//    "init": {"device_type": 82, "sd_req": [182], "crc16": 4660},   // optional
//    "name": "...", "created": "...", "notes": "..."}                 // optional
//
// File URLs resolve against the manifest URL. Unknown keys are ignored.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dfu32_core/errors.hpp"
#include "dfu_proto/init_packet.hpp"

namespace dfu32 {

inline constexpr size_t kMaxManifestBytes = 8 * 1024;
inline constexpr uint32_t kManifestSchema = 1;
// Field limits follow the staging header (dfu_proto/staging_format.hpp).
inline constexpr size_t kMaxVersionLen = 63;
inline constexpr size_t kMaxBoardLen = 31;
inline constexpr size_t kMaxUrlLen = 255;

using Sha256 = std::array<uint8_t, 32>;

struct ManifestFile {
    std::string url;  // absolute (resolved against the manifest URL)
    uint32_t size = 0;
    Sha256 sha256{};
};

struct ManifestInit {
    bool present = false;
    bool has_device_type = false;
    uint16_t device_type = 0;
    bool has_sd_req = false;
    std::vector<uint16_t> sd_req;
    bool has_crc16 = false;
    uint16_t crc16 = 0;
};

struct Manifest {
    uint32_t schema = 0;
    std::string name;
    std::string version;
    std::vector<std::string> boards;
    std::string created;
    std::string notes;
    ManifestFile bin;
    ManifestFile dat;
    ManifestInit init;
};

/// Parses and validates a manifest fetched from `manifest_url`.
/// MANIFEST_UNSUPPORTED for schema > 1, MANIFEST_INVALID for anything else
/// malformed, DAT_TOO_LONG if dat.size exceeds the 64-byte staging slot.
Status parse_manifest(std::string_view text, std::string_view manifest_url, Manifest& out);

/// `board` is listed in manifest.boards (exact, ASCII case-insensitive).
bool board_matches(const Manifest& m, std::string_view board) noexcept;

/// A staged image whose header names `image_board` may be flashed to a target
/// whose board is `target_board`. Checked only when both are set (ASCII
/// case-insensitive): an image staged over USB from the installer page carries
/// no board, so the operator's choice of package is the check (SPEC §8).
bool staged_board_ok(std::string_view image_board, std::string_view target_board) noexcept;

/// Cross-checks a parsed .dat against the manifest's optional `init` block.
/// DAT_INVALID on any disagreement (a server-side .dat mix-up).
Status check_init_against_manifest(const Manifest& m, const dfu::InitPacket& init);

/// Lowercase hex <-> bytes for SHA-256 digests.
bool parse_sha256_hex(std::string_view hex, Sha256& out) noexcept;
std::string sha256_hex(std::span<const uint8_t, 32> digest);

}  // namespace dfu32
