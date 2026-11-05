// SPDX-License-Identifier: MIT
//
// ESP-IDF implementations of dfu32_core's staging-writer interfaces: the raw
// `staging` partition and a PSA (hardware-accelerated) SHA-256.
#pragma once

#include "dfu32_core/stage_writer.hpp"
#include "esp_partition.h"
#include "psa/crypto.h"

namespace dfu32 {

class PartitionRegion final : public IFlashRegion {
public:
    explicit PartitionRegion(const esp_partition_t* p) : part_(p) {}
    /// The `staging` partition, or nullptr.
    static const esp_partition_t* find_staging();

    uint32_t size() const override { return part_->size; }
    bool erase(uint32_t offset, uint32_t len) override;
    bool write(uint32_t offset, std::span<const uint8_t> data) override;
    bool read(uint32_t offset, std::span<uint8_t> out) override;

private:
    const esp_partition_t* part_;
};

class PsaSha256 final : public ISha256 {
public:
    ~PsaSha256() override { psa_hash_abort(&op_); }
    bool reset() override;
    bool update(std::span<const uint8_t> data) override;
    bool finish(Sha256& out) override;

private:
    psa_hash_operation_t op_ = psa_hash_operation_init();
};

}  // namespace dfu32
