// SPDX-License-Identifier: MIT
//
// Writes a manifest's image into the raw staging partition (SPEC.md §8):
//
//   1. prechecks on the manifest and the already-downloaded .dat (§7.2);
//      nothing is erased if these fail, so the previous image survives
//   2. erase the header sector (invalidates the old image)
//   3. stream the .bin to 0x1000+, erasing sectors just ahead of it and
//      hashing as it arrives
//   4. read back and re-hash; CRC16 against the .dat
//   5. write the header last, so a valid header always means a valid image
//
// Pure C++ over two small interfaces so it runs in host tests; the firmware
// supplies an esp_partition region and a PSA SHA-256.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "dfu32_core/errors.hpp"
#include "dfu32_core/manifest.hpp"
#include "dfu_proto/init_packet.hpp"
#include "dfu_proto/staging_format.hpp"

namespace dfu32 {

class IFlashRegion {
public:
    virtual ~IFlashRegion() = default;
    virtual uint32_t size() const = 0;
    /// `offset` and `len` are multiples of kFlashSector.
    virtual bool erase(uint32_t offset, uint32_t len) = 0;
    virtual bool write(uint32_t offset, std::span<const uint8_t> data) = 0;
    virtual bool read(uint32_t offset, std::span<uint8_t> out) = 0;
};

class ISha256 {
public:
    virtual ~ISha256() = default;
    virtual bool reset() = 0;
    virtual bool update(std::span<const uint8_t> data) = 0;
    virtual bool finish(Sha256& out) = 0;
};

inline constexpr uint32_t kFlashSector = 4096;

/// Who the image is staged for, recorded in the header.
struct StageMeta {
    std::string target_id;     // <= 31
    std::string board;         // <= 31; must be in manifest.boards
    std::string manifest_url;  // <= 255
    Sha256 manifest_sha256{};
    uint64_t staged_at = 0;    // unix seconds, 0 if SNTP has no time yet
    uint32_t max_app_size = 0; // 0 = dfu::default_max_app_size(.dat device type)
};

/// The §7.2 checks that need only the manifest and the .dat: board, .dat
/// size/hash/layout/device type, manifest `init` cross-check, .bin alignment
/// and size against the target and the staging capacity (`region_size`).
/// Fills `init` with the parsed .dat on success.
Status precheck_image(const Manifest& m, std::span<const uint8_t> dat, const StageMeta& meta, uint32_t region_size,
                      ISha256& sha, dfu::InitPacket& init);

/// The staged header describes exactly this manifest's .bin and .dat.
bool same_image(const Manifest& m, const dfu::StagingHeader& staged) noexcept;

/// An already-staged image is being reused for `meta` (another target id,
/// board or manifest URL may now own it). Rewrites the header sector when
/// target_id, board, version, manifest_url or manifest_sha256 differ, so the
/// header never attributes the image to the wrong target; the image data is
/// untouched. `rewritten` says whether a write happened.
Status refresh_staged_header(IFlashRegion& region, const dfu::StagingHeader& current, const Manifest& m,
                             const StageMeta& meta, bool& rewritten);

class StagingWriter {
public:
    StagingWriter(IFlashRegion& region, ISha256& sha) : region_(region), sha_(sha) {}

    /// Prechecks, then erases the header sector.
    Status begin(const Manifest& m, std::span<const uint8_t> dat, const StageMeta& meta);
    /// Appends .bin bytes. SIZE_MISMATCH if the body runs past manifest.bin.size.
    Status write(std::span<const uint8_t> chunk);
    /// Size and hash of what arrived, read-back verification, CRC16, header.
    Status finish();

    uint32_t written() const noexcept { return written_; }
    /// The previous image's header has been (or may have been) erased.
    bool invalidated_previous() const noexcept { return invalidated_; }
    uint32_t total() const noexcept { return header_.bin_size; }
    const dfu::InitPacket& init() const noexcept { return init_; }
    const dfu::StagingHeader& header() const noexcept { return header_; }

private:
    enum class State : uint8_t { Idle, Writing, Done, Failed };
    Status fail(Status s);

    IFlashRegion& region_;
    ISha256& sha_;
    State state_ = State::Idle;
    uint32_t written_ = 0;
    uint32_t erased_to_ = 0;  // data bytes erased so far (sector multiple)
    bool invalidated_ = false;
    dfu::InitPacket init_{};
    dfu::StagingHeader header_{};
};

}  // namespace dfu32
