// SPDX-License-Identifier: MIT
#include "dfu_proto/staging_format.hpp"

#include <algorithm>
#include <cstring>

namespace dfu {
namespace {

constexpr char kMagic[8] = {'D', 'F', '3', '2', 'S', 'T', 'G', '1'};

constexpr size_t kOffVersion = 0x008;
constexpr size_t kOffBinSize = 0x00C;
constexpr size_t kOffBinSha = 0x010;
constexpr size_t kOffDatSize = 0x030;
constexpr size_t kOffDat = 0x032;
constexpr size_t kOffDatSha = 0x072;
constexpr size_t kOffCrc16 = 0x092;
constexpr size_t kOffTargetId = 0x094;
constexpr size_t kOffBoard = 0x0B4;
constexpr size_t kOffVer = 0x0D4;
constexpr size_t kOffManifestSha = 0x114;
constexpr size_t kOffStagedAt = 0x134;
constexpr size_t kOffUrl = 0x13C;
constexpr size_t kOffCrc32 = 0x23C;

void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get_u32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

bool put_str(uint8_t* p, size_t field, const std::string& s) {
    if (s.size() >= field) return false;  // keep at least one NUL
    std::memset(p, 0, field);
    std::memcpy(p, s.data(), s.size());
    return true;
}
std::string get_str(const uint8_t* p, size_t field) {
    const auto* end = static_cast<const uint8_t*>(std::memchr(p, 0, field));
    return std::string(reinterpret_cast<const char*>(p), end ? static_cast<size_t>(end - p) : field);
}

}  // namespace

const char* to_string(StagingHeaderError e) noexcept {
    switch (e) {
        case StagingHeaderError::Ok: return "OK";
        case StagingHeaderError::TooShort: return "TOO_SHORT";
        case StagingHeaderError::Erased: return "EMPTY";
        case StagingHeaderError::BadMagic: return "BAD_MAGIC";
        case StagingHeaderError::UnsupportedVersion: return "UNSUPPORTED_VERSION";
        case StagingHeaderError::BadCrc: return "BAD_CRC";
        case StagingHeaderError::BadDatSize: return "BAD_DAT_SIZE";
        case StagingHeaderError::FieldTooLong: return "FIELD_TOO_LONG";
    }
    return "UNKNOWN";
}

uint32_t crc32_ieee(std::span<const uint8_t> data, uint32_t crc) noexcept {
    crc = ~crc;
    for (const uint8_t b : data) {
        crc ^= b;
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

StagingHeaderError encode_staging_header(const StagingHeader& h, std::span<uint8_t, kStagingHeaderSize> out) noexcept {
    if (h.dat_size == 0 || h.dat_size > kStagingMaxDat) return StagingHeaderError::BadDatSize;
    uint8_t* p = out.data();
    std::memset(p, 0, kStagingHeaderSize);
    std::memcpy(p, kMagic, sizeof(kMagic));
    put_u32(p + kOffVersion, kStagingHeaderVersion);
    put_u32(p + kOffBinSize, h.bin_size);
    std::memcpy(p + kOffBinSha, h.bin_sha256.data(), 32);
    put_u16(p + kOffDatSize, h.dat_size);
    std::memcpy(p + kOffDat, h.dat.data(), kStagingMaxDat);
    std::memcpy(p + kOffDatSha, h.dat_sha256.data(), 32);
    put_u16(p + kOffCrc16, h.crc16);
    if (!put_str(p + kOffTargetId, 32, h.target_id) || !put_str(p + kOffBoard, 32, h.board) ||
        !put_str(p + kOffVer, 64, h.version) || !put_str(p + kOffUrl, 256, h.manifest_url)) {
        return StagingHeaderError::FieldTooLong;
    }
    std::memcpy(p + kOffManifestSha, h.manifest_sha256.data(), 32);
    put_u64(p + kOffStagedAt, h.staged_at);
    put_u32(p + kOffCrc32, crc32_ieee({p, kOffCrc32}));
    return StagingHeaderError::Ok;
}

StagingHeaderError decode_staging_header(std::span<const uint8_t> in, StagingHeader& out) {
    if (in.size() < kStagingHeaderSize) return StagingHeaderError::TooShort;
    const uint8_t* p = in.data();
    if (std::all_of(p, p + kStagingHeaderSize, [](uint8_t b) { return b == 0xFF; })) {
        return StagingHeaderError::Erased;
    }
    if (std::memcmp(p, kMagic, sizeof(kMagic)) != 0) return StagingHeaderError::BadMagic;
    if (get_u32(p + kOffVersion) != kStagingHeaderVersion) return StagingHeaderError::UnsupportedVersion;
    if (get_u32(p + kOffCrc32) != crc32_ieee({p, kOffCrc32})) return StagingHeaderError::BadCrc;
    const uint16_t dat_size = get_u16(p + kOffDatSize);
    if (dat_size == 0 || dat_size > kStagingMaxDat) return StagingHeaderError::BadDatSize;

    StagingHeader h;
    h.bin_size = get_u32(p + kOffBinSize);
    std::memcpy(h.bin_sha256.data(), p + kOffBinSha, 32);
    h.dat_size = dat_size;
    std::memcpy(h.dat.data(), p + kOffDat, kStagingMaxDat);
    std::memcpy(h.dat_sha256.data(), p + kOffDatSha, 32);
    h.crc16 = get_u16(p + kOffCrc16);
    h.target_id = get_str(p + kOffTargetId, 32);
    h.board = get_str(p + kOffBoard, 32);
    h.version = get_str(p + kOffVer, 64);
    std::memcpy(h.manifest_sha256.data(), p + kOffManifestSha, 32);
    h.staged_at = get_u64(p + kOffStagedAt);
    h.manifest_url = get_str(p + kOffUrl, 256);
    out = std::move(h);
    return StagingHeaderError::Ok;
}

}  // namespace dfu
