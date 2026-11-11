// SPDX-License-Identifier: MIT
#include "dfu32_core/ota_image.hpp"

#include <cstring>

namespace dfu32::ota {
namespace {

std::string fixed_string(std::span<const uint8_t> field) {
    size_t n = 0;
    while (n < field.size() && field[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(field.data()), n);
}

uint32_t le32(const uint8_t* p) {
    return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24;
}

const char* chip_name(uint16_t id) {
    switch (id) {
        case 0x0000: return "ESP32";
        case 0x0002: return "ESP32-S2";
        case 0x0005: return "ESP32-C3";
        case 0x0009: return "ESP32-S3";
        case 0x000C: return "ESP32-C2";
        case 0x000D: return "ESP32-C6";
        case 0x0010: return "ESP32-H2";
        case 0x0012: return "ESP32-P4";
        case 0x0014: return "ESP32-C61";
        case 0x0017: return "ESP32-C5";
        default: return nullptr;
    }
}

std::string chip_text(uint16_t id) {
    const char* n = chip_name(id);
    return n != nullptr ? std::string(n) : "chip id " + std::to_string(id);
}

}  // namespace

std::string check_image_start(std::span<const uint8_t> head, const Expect& expect, ImageInfo& out) {
    out = ImageInfo{};
    if (head.size() < kInspectBytes) return "image too short to be DFU32 firmware";
    if (head[0] != kImageMagic) return "not an ESP-IDF app image (use dfu32.bin, not the merged image)";
    out.chip_id = static_cast<uint16_t>(head[12] | head[13] << 8);
    const uint8_t* desc = head.data() + kAppDescOffset;
    if (le32(desc) != kAppDescMagic) return "app description missing (not an ESP-IDF app image)";
    out.version = fixed_string(head.subspan(kAppDescOffset + 16, 32));
    out.project = fixed_string(head.subspan(kAppDescOffset + 48, 32));
    if (out.chip_id != expect.chip_id) {
        return "image is for " + chip_text(out.chip_id) + ", this device is " + chip_text(expect.chip_id);
    }
    if (out.project != expect.project) {
        return "image is the project \"" + out.project + "\", not \"" + std::string(expect.project) + "\"";
    }
    const uint8_t* tag = head.data() + kBoardTagOffset;
    if (std::memcmp(tag, kBoardTagMagic, sizeof(kBoardTagMagic)) == 0) {
        out.board = fixed_string(head.subspan(kBoardTagOffset + sizeof(kBoardTagMagic), sizeof(BoardTag::board)));
    }
    if (expect.allow_other_board) return {};
    if (out.board.empty()) {
        return "image has no DFU32 board tag (built before v0.4?); add ?force=1 to install it anyway";
    }
    if (out.board != expect.board) {
        return "image is for board \"" + out.board + "\", this device is \"" + std::string(expect.board) +
               "\" (same chip, different flash layout or pins); add ?force=1 to install it anyway";
    }
    return {};
}

}  // namespace dfu32::ota
