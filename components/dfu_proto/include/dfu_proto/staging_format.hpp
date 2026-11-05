// SPDX-License-Identifier: MIT
//
// On-flash format of the raw `staging` partition (SPEC.md §8). The header
// occupies the first 4 KiB sector and is written last, so a header that
// decodes with a valid CRC means the image after it was fully written and
// verified. The application image starts at kStagingDataOffset.
//
//   0x000  char[8]   magic "DF32STG1"
//   0x008  u32       header_version = 1
//   0x00C  u32       bin_size
//   0x010  u8[32]    bin_sha256
//   0x030  u16       dat_size (<= 64)
//   0x032  u8[64]    dat bytes
//   0x072  u8[32]    dat_sha256
//   0x092  u16       crc16 (from .dat)
//   0x094  char[32]  target_id
//   0x0B4  char[32]  board
//   0x0D4  char[64]  version
//   0x114  u8[32]    manifest_sha256
//   0x134  u64       staged_at (unix seconds, 0 = unknown)
//   0x13C  char[256] manifest_url
//   0x23C  u32       header_crc32 (IEEE, over 0x000..0x23B)
//
// All integers little-endian; strings NUL-padded. tools/dfu32_stage_image.py
// writes the same layout.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace dfu {

inline constexpr size_t kStagingHeaderSize = 0x240;
inline constexpr uint32_t kStagingDataOffset = 0x1000;
inline constexpr uint32_t kStagingHeaderVersion = 1;
inline constexpr size_t kStagingMaxDat = 64;

struct StagingHeader {
    uint32_t bin_size = 0;
    std::array<uint8_t, 32> bin_sha256{};
    uint16_t dat_size = 0;
    std::array<uint8_t, kStagingMaxDat> dat{};
    std::array<uint8_t, 32> dat_sha256{};
    uint16_t crc16 = 0;
    std::string target_id;     // <= 31 chars
    std::string board;         // <= 31 chars
    std::string version;       // <= 63 chars
    std::array<uint8_t, 32> manifest_sha256{};
    uint64_t staged_at = 0;
    std::string manifest_url;  // <= 255 chars

    std::span<const uint8_t> dat_bytes() const { return {dat.data(), dat_size}; }
};

enum class StagingHeaderError : uint8_t {
    Ok,
    TooShort,
    Erased,              // header sector blank: nothing staged
    BadMagic,
    UnsupportedVersion,
    BadCrc,
    BadDatSize,
    FieldTooLong,        // encode only
};

const char* to_string(StagingHeaderError e) noexcept;

/// CRC-32/ISO-HDLC (zlib crc32). Pass the previous result to continue.
uint32_t crc32_ieee(std::span<const uint8_t> data, uint32_t crc = 0) noexcept;

StagingHeaderError encode_staging_header(const StagingHeader& h, std::span<uint8_t, kStagingHeaderSize> out) noexcept;
StagingHeaderError decode_staging_header(std::span<const uint8_t> in, StagingHeader& out);

}  // namespace dfu
