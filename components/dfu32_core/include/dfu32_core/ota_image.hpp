// SPDX-License-Identifier: MIT
//
// Sanity checks on an uploaded DFU32 firmware image before it is written to
// the other OTA slot (POST /api/v1/system/ota, SPEC.md §10.1). ESP-IDF's
// esp_ota_end() verifies the image itself (checksum, SHA-256); this refuses,
// up front, an image for another chip, another project, or another DFU32
// board profile (same chip, different flash layout and pins). Pure, so the
// byte offsets are host-tested.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dfu32::ota {

// ESP app image layout (esp_image_header_t, esp_image_segment_header_t,
// esp_app_desc_t, then the optional custom descriptor in .rodata_custom_desc).
inline constexpr size_t kImageHeaderSize = 24;
inline constexpr size_t kSegmentHeaderSize = 8;
inline constexpr size_t kAppDescOffset = kImageHeaderSize + kSegmentHeaderSize;  // 32
inline constexpr size_t kAppDescSize = 256;
inline constexpr size_t kBoardTagOffset = kAppDescOffset + kAppDescSize;       // 288
inline constexpr uint8_t kImageMagic = 0xE9;
inline constexpr uint32_t kAppDescMagic = 0xABCD5432;

/// DFU32's custom app descriptor: which board profile the image was built
/// for. Placed in .rodata_custom_desc, right after esp_app_desc_t.
struct BoardTag {
    char magic[8];   // "DF32BRD1", no terminator
    char board[32];  // CONFIG_DFU32_BOARD_NAME, NUL-padded
};
static_assert(sizeof(BoardTag) == 40);
inline constexpr char kBoardTagMagic[8] = {'D', 'F', '3', '2', 'B', 'R', 'D', '1'};

/// Bytes check_image_start() needs.
inline constexpr size_t kInspectBytes = kBoardTagOffset + sizeof(BoardTag);  // 328

struct ImageInfo {
    uint16_t chip_id = 0;
    std::string version;
    std::string project;
    std::string board;      // empty when the image has no DFU32 board tag
};

struct Expect {
    uint16_t chip_id = 0;
    std::string_view project;  // e.g. "dfu32"
    std::string_view board;    // e.g. "feather_esp32s3"
    bool allow_other_board = false;  // ?force=1: skip the board check only
};

/// Parses the first kInspectBytes of an image. Returns "" when it may be
/// written, else why not. `out` is filled as far as parsing got.
std::string check_image_start(std::span<const uint8_t> head, const Expect& expect, ImageInfo& out);

}  // namespace dfu32::ota
