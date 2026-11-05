// SPDX-License-Identifier: MIT
#include "dfu32_core/stage_writer.hpp"

#include <algorithm>
#include <array>
#include <vector>

#include "dfu_proto/crc16.hpp"
#include "dfu_proto/image.hpp"

namespace dfu32 {
namespace {

uint32_t round_up_sector(uint32_t n) { return (n + kFlashSector - 1) / kFlashSector * kFlashSector; }

bool hash_of(ISha256& sha, std::span<const uint8_t> data, Sha256& out) {
    return sha.reset() && sha.update(data) && sha.finish(out);
}

std::string bytes(uint64_t n) { return std::to_string(n) + " B"; }

// Header sector must already be erased. Writes, reads back, decodes.
Status write_header(IFlashRegion& region, const dfu::StagingHeader& h) {
    std::array<uint8_t, dfu::kStagingHeaderSize> hdr{};
    if (dfu::encode_staging_header(h, hdr) != dfu::StagingHeaderError::Ok) {
        return Status::fail(Err::StagingIo, "header encode failed");
    }
    if (!region.write(0, hdr)) return Status::fail(Err::StagingIo, "header write failed");
    std::array<uint8_t, dfu::kStagingHeaderSize> check{};
    dfu::StagingHeader decoded;
    if (!region.read(0, check) || check != hdr ||
        dfu::decode_staging_header(check, decoded) != dfu::StagingHeaderError::Ok) {
        return Status::fail(Err::StagingIo, "header read-back failed");
    }
    return {};
}

}  // namespace

Status precheck_image(const Manifest& m, std::span<const uint8_t> dat, const StageMeta& meta, uint32_t region_size,
                      ISha256& sha, dfu::InitPacket& init) {
    if (!board_matches(m, meta.board)) {
        std::string listed;
        for (const auto& b : m.boards) listed += (listed.empty() ? "" : ", ") + b;
        return Status::fail(Err::BoardMismatch, "target board '" + meta.board + "' not in manifest boards [" + listed + "]");
    }
    if (meta.target_id.size() > 31 || meta.board.size() > 31 || meta.manifest_url.size() > kMaxUrlLen) {
        return Status::fail(Err::ManifestInvalid, "target id, board or manifest URL too long for the staging header");
    }

    // .dat
    if (dat.size() > dfu::kStagingMaxDat) return Status::fail(Err::DatTooLong, ".dat is " + bytes(dat.size()));
    if (dat.size() != m.dat.size) {
        return Status::fail(Err::SizeMismatch, ".dat is " + bytes(dat.size()) + ", manifest says " + bytes(m.dat.size));
    }
    Sha256 digest{};
    if (!hash_of(sha, dat, digest)) return Status::fail(Err::StagingIo, "SHA-256 failed");
    if (digest != m.dat.sha256) return Status::fail(Err::HashMismatch, ".dat SHA-256 differs from the manifest");
    const dfu::InitPacketError pe = dfu::parse_init_packet(dat, init);
    if (pe != dfu::InitPacketError::Ok) return Status::fail(Err::DatInvalid, dfu::to_string(pe));
    if (init.device_type != dfu::kAdafruitDeviceType && init.device_type != dfu::kNrf54DeviceType) {
        return Status::fail(Err::DatInvalid, "unknown device type " + std::to_string(init.device_type) +
                                                 " (want 0x0052 nRF52 or 0x0054 nRF54)");
    }
    if (Status s = check_init_against_manifest(m, init); !s.ok()) return s;

    // .bin
    if ((m.bin.size & 3u) != 0) {
        return Status::fail(Err::ImageNotWordAligned, ".bin is " + bytes(m.bin.size) + ", not a multiple of 4");
    }
    const uint32_t max_app = meta.max_app_size ? meta.max_app_size : dfu::default_max_app_size(init.device_type);
    if (m.bin.size > max_app) {
        return Status::fail(Err::ImageTooLarge, ".bin is " + bytes(m.bin.size) + ", target limit " + bytes(max_app));
    }
    if (uint64_t{dfu::kStagingDataOffset} + m.bin.size > region_size) {
        return Status::fail(Err::ImageTooLarge, ".bin is " + bytes(m.bin.size) + ", staging holds " +
                                                    bytes(region_size - dfu::kStagingDataOffset));
    }
    return {};
}

bool same_image(const Manifest& m, const dfu::StagingHeader& staged) noexcept {
    return staged.bin_size == m.bin.size && staged.bin_sha256 == m.bin.sha256 && staged.dat_size == m.dat.size &&
           staged.dat_sha256 == m.dat.sha256;
}

Status refresh_staged_header(IFlashRegion& region, const dfu::StagingHeader& current, const Manifest& m,
                             const StageMeta& meta, bool& rewritten) {
    rewritten = false;
    if (current.target_id == meta.target_id && current.board == meta.board && current.version == m.version &&
        current.manifest_url == meta.manifest_url && current.manifest_sha256 == meta.manifest_sha256) {
        return {};
    }
    dfu::StagingHeader h = current;
    h.target_id = meta.target_id;
    h.board = meta.board;
    h.version = m.version;
    h.manifest_url = meta.manifest_url;
    h.manifest_sha256 = meta.manifest_sha256;
    h.staged_at = meta.staged_at;
    std::array<uint8_t, dfu::kStagingHeaderSize> probe{};
    if (dfu::encode_staging_header(h, probe) != dfu::StagingHeaderError::Ok) {
        return Status::fail(Err::ManifestInvalid, "target id, board, version or URL too long for the staging header");
    }
    rewritten = true;  // from the erase on, the old header is gone
    if (!region.erase(0, kFlashSector)) return Status::fail(Err::StagingIo, "erasing the header failed");
    return write_header(region, h);
}

Status StagingWriter::fail(Status s) {
    state_ = State::Failed;
    return s;
}

Status StagingWriter::begin(const Manifest& m, std::span<const uint8_t> dat, const StageMeta& meta) {
    state_ = State::Failed;
    written_ = 0;
    invalidated_ = false;
    header_ = dfu::StagingHeader{};
    if (Status s = precheck_image(m, dat, meta, region_.size(), sha_, init_); !s.ok()) return s;

    header_.bin_size = m.bin.size;
    header_.bin_sha256 = m.bin.sha256;
    header_.dat_size = static_cast<uint16_t>(dat.size());
    std::copy(dat.begin(), dat.end(), header_.dat.begin());
    header_.dat_sha256 = m.dat.sha256;
    header_.crc16 = init_.firmware_crc16;
    header_.target_id = meta.target_id;
    header_.board = meta.board;
    header_.version = m.version;
    header_.manifest_sha256 = meta.manifest_sha256;
    header_.staged_at = meta.staged_at;
    header_.manifest_url = meta.manifest_url;

    // Header first: from here on the partition reads as "nothing staged".
    // Data sectors are erased as the body arrives (write()), so the HTTP
    // connection never sits idle through a multi-second bulk erase.
    invalidated_ = true;  // set before the erase: a failed erase may still have cleared it
    if (!region_.erase(0, kFlashSector)) return fail(Status::fail(Err::StagingIo, "erasing the header failed"));
    erased_to_ = 0;
    if (!sha_.reset()) return fail(Status::fail(Err::StagingIo, "SHA-256 failed"));
    state_ = State::Writing;
    return {};
}

Status StagingWriter::write(std::span<const uint8_t> chunk) {
    if (state_ != State::Writing) return Status::fail(Err::StagingIo, "writer not started");
    if (chunk.empty()) return {};
    if (uint64_t{written_} + chunk.size() > header_.bin_size) {
        return fail(Status::fail(Err::SizeMismatch, ".bin is longer than the manifest's " + bytes(header_.bin_size)));
    }
    const uint32_t end = written_ + static_cast<uint32_t>(chunk.size());
    if (end > erased_to_) {
        const uint32_t len = round_up_sector(end) - erased_to_;
        if (!region_.erase(dfu::kStagingDataOffset + erased_to_, len)) {
            return fail(Status::fail(Err::StagingIo, "flash erase failed at " + bytes(erased_to_)));
        }
        erased_to_ += len;
    }
    if (!region_.write(dfu::kStagingDataOffset + written_, chunk)) {
        return fail(Status::fail(Err::StagingIo, "flash write failed at " + bytes(written_)));
    }
    if (!sha_.update(chunk)) return fail(Status::fail(Err::StagingIo, "SHA-256 failed"));
    written_ += static_cast<uint32_t>(chunk.size());
    return {};
}

Status StagingWriter::finish() {
    if (state_ != State::Writing) return Status::fail(Err::StagingIo, "writer not started");
    if (written_ != header_.bin_size) {
        return fail(Status::fail(Err::SizeMismatch,
                                 ".bin is " + bytes(written_) + ", manifest says " + bytes(header_.bin_size)));
    }
    Sha256 streamed{};
    if (!sha_.finish(streamed)) return fail(Status::fail(Err::StagingIo, "SHA-256 failed"));
    if (streamed != header_.bin_sha256) {
        return fail(Status::fail(Err::HashMismatch, ".bin SHA-256 differs from the manifest"));
    }

    // Read back: catches flash write errors before the header vouches for it.
    std::vector<uint8_t> buf(kFlashSector);
    uint16_t crc = dfu::kCrc16Init;
    if (!sha_.reset()) return fail(Status::fail(Err::StagingIo, "SHA-256 failed"));
    for (uint32_t off = 0; off < header_.bin_size; off += kFlashSector) {
        const size_t n = std::min<size_t>(kFlashSector, header_.bin_size - off);
        const std::span<uint8_t> part(buf.data(), n);
        if (!region_.read(dfu::kStagingDataOffset + off, part) || !sha_.update(part)) {
            return fail(Status::fail(Err::StagingIo, "read-back failed at " + bytes(off)));
        }
        crc = dfu::crc16_ccitt(part, crc);
    }
    Sha256 readback{};
    if (!sha_.finish(readback)) return fail(Status::fail(Err::StagingIo, "SHA-256 failed"));
    if (readback != streamed) return fail(Status::fail(Err::StagingIo, "read-back differs from what was written"));
    if (crc != init_.firmware_crc16) {
        return fail(Status::fail(Err::DatCrcMismatch, "CRC16 of the .bin does not match the .dat"));
    }

    if (Status hs = write_header(region_, header_); !hs.ok()) return fail(std::move(hs));
    state_ = State::Done;
    return {};
}

}  // namespace dfu32
