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
    bool is_app = false;    // it has an ESP-IDF app header and esp_app_desc_t
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

// A merged flash image (idf.py merge-bin, the release's -merged.bin) starts
// with the bootloader at the chip's offset, 0x0, 0x1000 (ESP32, S2) or
// 0x2000 (C5), with 0xFF before it, and has the partition table at 0x8000.
inline constexpr size_t kPartitionTableOffset = 0x8000;
inline constexpr uint8_t kPartitionMagic[2] = {0xAA, 0x50};  // ESP_PARTITION_MAGIC, little-endian
/// Bytes MergedProbe needs to see.
inline constexpr size_t kMergedProbeBytes = kPartitionTableOffset + sizeof(kPartitionMagic);

/// Tells a merged flash image from other non-app uploads, from the bytes at
/// a few offsets, so the caller can stream the body through it in chunks
/// without holding 32 KB.
class MergedProbe {
public:
    /// `chunk` holds the image's bytes from `offset` on. Chunks may come in
    /// any size, in order.
    void feed(size_t offset, std::span<const uint8_t> chunk);
    /// A bootloader at one of the bootloader offsets (and only 0xFF before
    /// it), and a partition table at 0x8000.
    bool merged() const;

private:
    uint8_t at_0_ = 0, at_1000_ = 0, at_2000_ = 0;
    uint8_t pt_[2] = {0, 0};
    bool ff_to_1000_ = true;  // bytes 0x1..0xFFF are all 0xFF
    bool ff_to_2000_ = true;  // bytes 0x1..0x1FFF, except 0x1000, are all 0xFF
    size_t seen_ = 0;         // bytes fed so far, from 0
};

/// The refusal for a merged image, naming the file to upload instead.
std::string merged_image_refusal(std::string_view board);

}  // namespace dfu32::ota
