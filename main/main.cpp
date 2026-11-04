// SPDX-License-Identifier: MIT
//
// DFU32 M0 skeleton: boot banner and layout self-check. The DFU client,
// staging, networking and API arrive in later milestones (docs/MILESTONES.md).
#include <cinttypes>
#include <cstdint>

#include "dfu_proto/chunk.hpp"
#include "dfu_proto/image.hpp"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

constexpr const char* TAG = "dfu32";

// Custom data partition subtypes (partitions/*.csv, SPEC.md §8, §12).
constexpr auto kSubtypeStaging = static_cast<esp_partition_subtype_t>(0x40);
constexpr auto kSubtypeRunlog = static_cast<esp_partition_subtype_t>(0x41);

constexpr uint32_t kStagingHeaderSize = 4096;
constexpr uint32_t kRunlogSlotSize = 4096;
constexpr uint32_t kMinRunlogSlots = 10;

bool check_data_partition(const char* label, esp_partition_subtype_t subtype, uint32_t min_size) {
    const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, label);
    if (p == nullptr) {
        ESP_LOGE(TAG, "partition '%s' (subtype 0x%02x) missing", label, static_cast<unsigned>(subtype));
        return false;
    }
    ESP_LOGI(TAG, "partition %-8s @ 0x%06" PRIx32 " size %" PRIu32, label, p->address, p->size);
    if (p->size < min_size) {
        ESP_LOGE(TAG, "partition '%s' too small: %" PRIu32 " < %" PRIu32, label, p->size, min_size);
        return false;
    }
    return true;
}

void init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

}  // namespace

extern "C" void app_main(void) {
    const esp_app_desc_t* app = esp_app_get_description();
    ESP_LOGI(TAG, "DFU32 %s (ESP-IDF %s), board profile '%s'", app->version, app->idf_ver,
             CONFIG_DFU32_BOARD_NAME);

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip %s rev v%d.%d, %d core(s), PSRAM %u bytes", CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100, chip.cores,
             static_cast<unsigned>(esp_psram_get_size()));

    init_nvs();

    bool ok = true;
    ok &= check_data_partition("staging", kSubtypeStaging, kStagingHeaderSize + dfu::kMaxAppSizeS140v6);
    ok &= check_data_partition("runlog", kSubtypeRunlog, kMinRunlogSlots * kRunlogSlotSize);

    ESP_LOGI(TAG, "dfu_proto: packet size at MTU 23 = %u, at MTU 247 = %u",
             static_cast<unsigned>(dfu::packet_size_for_mtu(23)),
             static_cast<unsigned>(dfu::packet_size_for_mtu(247)));

    if (!ok) {
        ESP_LOGE(TAG, "layout self-check FAILED; refusing to mark this image valid");
        return;
    }

    // M0 has no network, so mark valid once the layout self-check passes.
    // M3 moves this after Wi-Fi + HTTP come up (SPEC.md §12.3, rollback).
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "layout self-check OK");
}
