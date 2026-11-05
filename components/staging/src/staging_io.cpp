// SPDX-License-Identifier: MIT
#include "staging/staging_io.hpp"

#include "staging/staging.hpp"

namespace dfu32 {

const esp_partition_t* PartitionRegion::find_staging() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kPartitionSubtypeStaging, "staging");
}

bool PartitionRegion::erase(uint32_t offset, uint32_t len) {
    return esp_partition_erase_range(part_, offset, len) == ESP_OK;
}

bool PartitionRegion::write(uint32_t offset, std::span<const uint8_t> data) {
    return esp_partition_write(part_, offset, data.data(), data.size()) == ESP_OK;
}

bool PartitionRegion::read(uint32_t offset, std::span<uint8_t> out) {
    return esp_partition_read(part_, offset, out.data(), out.size()) == ESP_OK;
}

bool PsaSha256::reset() {
    psa_hash_abort(&op_);
    op_ = psa_hash_operation_init();
    return psa_hash_setup(&op_, PSA_ALG_SHA_256) == PSA_SUCCESS;
}

bool PsaSha256::update(std::span<const uint8_t> data) {
    return psa_hash_update(&op_, data.data(), data.size()) == PSA_SUCCESS;
}

bool PsaSha256::finish(Sha256& out) {
    size_t len = 0;
    return psa_hash_finish(&op_, out.data(), out.size(), &len) == PSA_SUCCESS && len == out.size();
}

}  // namespace dfu32
