// SPDX-License-Identifier: MIT
//
// Read side of the raw `staging` partition (SPEC.md §8). In M1 the partition is
// written from a PC with tools/dfu32_stage_image.py; M3 adds the HTTP writer.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "dfu_proto/image.hpp"
#include "dfu_proto/init_packet.hpp"
#include "dfu_proto/staging_format.hpp"
#include "dfu_proto/transfer.hpp"
#include "esp_partition.h"

namespace dfu32 {

inline constexpr auto kPartitionSubtypeStaging = static_cast<esp_partition_subtype_t>(0x40);

struct StagingVerify {
    bool ok = false;
    std::string error;      // first failure, empty when ok
    bool sha256_ok = false;
    uint16_t bin_crc16 = 0;
    dfu::InitPacket init{};
};

class StagedImage final : public dfu::IImageSource {
public:
    /// Finds the partition and decodes the header. Returns false if the
    /// partition is missing; header problems are reported by header_status().
    bool open();

    const esp_partition_t* partition() const { return part_; }
    dfu::StagingHeaderError header_status() const { return status_; }
    const dfu::StagingHeader& header() const { return header_; }

    /// Re-reads the whole image: SHA-256 and CRC16 of the .bin, SHA-256 and
    /// layout of the .dat, then the pre-START checks (SPEC.md §7.2).
    StagingVerify verify(uint32_t max_app_size = dfu::kDefaultMaxAppSize);

    bool read(uint32_t offset, std::span<uint8_t> out) override;

private:
    const esp_partition_t* part_ = nullptr;
    dfu::StagingHeaderError status_ = dfu::StagingHeaderError::Erased;
    dfu::StagingHeader header_{};
};

}  // namespace dfu32
