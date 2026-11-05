// SPDX-License-Identifier: MIT
#include "staging/staging.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "dfu_proto/crc16.hpp"
#include "esp_log.h"
#include "psa/crypto.h"

namespace dfu32 {
namespace {

constexpr const char* TAG = "staging";
constexpr size_t kChunk = 4096;

bool sha256(std::span<const uint8_t> data, std::array<uint8_t, 32>& out) {
    size_t len = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, data.data(), data.size(), out.data(), out.size(), &len) ==
               PSA_SUCCESS &&
           len == out.size();
}

}  // namespace

bool StagedImage::open() {
    part_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kPartitionSubtypeStaging, "staging");
    if (part_ == nullptr) {
        ESP_LOGE(TAG, "no 'staging' partition");
        return false;
    }
    std::array<uint8_t, dfu::kStagingHeaderSize> buf{};
    if (esp_partition_read(part_, 0, buf.data(), buf.size()) != ESP_OK) {
        status_ = dfu::StagingHeaderError::TooShort;
        return true;
    }
    status_ = dfu::decode_staging_header(buf, header_);
    return true;
}

bool StagedImage::read(uint32_t offset, std::span<uint8_t> out) {
    if (part_ == nullptr || status_ != dfu::StagingHeaderError::Ok ||
        uint64_t{offset} + out.size() > header_.bin_size) {
        return false;
    }
    return esp_partition_read(part_, dfu::kStagingDataOffset + offset, out.data(), out.size()) == ESP_OK;
}

StagingVerify StagedImage::verify(uint32_t max_app_size) {
    StagingVerify v;
    if (part_ == nullptr) {
        v.error = "no staging partition";
        return v;
    }
    if (status_ != dfu::StagingHeaderError::Ok) {
        v.error = std::string("staging header: ") + dfu::to_string(status_);
        return v;
    }
    if (uint64_t{dfu::kStagingDataOffset} + header_.bin_size > part_->size) {
        v.error = "staged image larger than the partition";
        return v;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        v.error = "psa_crypto_init failed";
        return v;
    }

    // .dat: layout, hash, consistency with the header.
    const auto dat = header_.dat_bytes();
    if (dfu::parse_init_packet(dat, v.init) != dfu::InitPacketError::Ok) {
        v.error = "staged .dat is malformed";
        return v;
    }
    std::array<uint8_t, 32> digest{};
    if (!sha256(dat, digest) || digest != header_.dat_sha256) {
        v.error = "HASH_MISMATCH (.dat)";
        return v;
    }
    if (v.init.firmware_crc16 != header_.crc16) {
        v.error = "header CRC16 disagrees with .dat";
        return v;
    }

    // .bin: one pass for SHA-256 and CRC16.
    psa_hash_operation_t op = psa_hash_operation_init();
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        v.error = "psa_hash_setup failed";
        return v;
    }
    std::vector<uint8_t> buf(kChunk);
    uint16_t crc = dfu::kCrc16Init;
    for (uint32_t off = 0; off < header_.bin_size; off += kChunk) {
        const size_t n = std::min<size_t>(kChunk, header_.bin_size - off);
        if (esp_partition_read(part_, dfu::kStagingDataOffset + off, buf.data(), n) != ESP_OK) {
            psa_hash_abort(&op);
            v.error = "STAGING_IO: flash read failed";
            return v;
        }
        psa_hash_update(&op, buf.data(), n);
        crc = dfu::crc16_ccitt({buf.data(), n}, crc);
    }
    size_t len = 0;
    if (psa_hash_finish(&op, digest.data(), digest.size(), &len) != PSA_SUCCESS || len != digest.size()) {
        v.error = "psa_hash_finish failed";
        return v;
    }
    v.bin_crc16 = crc;
    v.sha256_ok = digest == header_.bin_sha256;
    if (!v.sha256_ok) {
        v.error = "HASH_MISMATCH (.bin)";
        return v;
    }

    if (max_app_size == 0) max_app_size = dfu::default_max_app_size(v.init.device_type);
    const dfu::ImageError e = dfu::check_app_image(v.init, header_.bin_size, crc, max_app_size);
    if (e != dfu::ImageError::Ok) {
        v.error = dfu::to_string(e);
        return v;
    }
    v.ok = true;
    return v;
}

}  // namespace dfu32
