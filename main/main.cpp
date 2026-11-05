// SPDX-License-Identifier: MIT
//
// DFU32 M1: boot banner, layout self-check, NimBLE central and the USB-serial
// bench console (scan / probe / flash). Networking and the HTTP API arrive in
// M3 (docs/MILESTONES.md).
#include <cinttypes>
#include <cstdint>

#include "console.hpp"
#include "dfu_ble/dfu_ble.hpp"
#include "dfu_proto/image.hpp"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include "runlog/run_record.hpp"
#include "runlog/runlog_idf.hpp"
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

// SPEC.md §7.3: a job marker that survived a reboot means the previous flash
// never finished. Record it so `runs` shows what happened.
static void record_aborted_run() {
    runlog::idf::JobMarker job;
    if (!runlog::idf::job_pending(job)) return;
    const bool maybe_erased = job.stage == runlog::idf::JobStage::Transferring;
    ESP_LOGW(TAG, "previous flash of %s (\"%s\") was interrupted by a reboot (%s)%s", job.target, job.version,
             runlog::idf::reset_reason_string(),
             maybe_erased ? "; the target's app may be erased: run `flash --yes` again" : "");
    runlog::RunLog* run_log = runlog::idf::open_run_log();
    if (run_log != nullptr && job.seq < run_log->next_seq()) {
        // The run was recorded; the reboot hit between append() and job_end().
        ESP_LOGI(TAG, "interrupted run #%u was already recorded", static_cast<unsigned>(job.seq));
    } else if (run_log != nullptr) {
        runlog::RunRecord rec;
        rec.seq = run_log->next_seq();
        rec.result = "ABORTED_BY_REBOOT";
        rec.error = "DFU32 rebooted during the run";
        rec.target = job.target;
        rec.image_version = job.version;
        rec.started_uptime_s = job.started_uptime_s;
        rec.target_erased = maybe_erased ? "possibly" : "no";
        rec.reset_reason = runlog::idf::reset_reason_string();
        run_log->append(runlog::to_json(rec), "");  // the interrupted boot's log is gone
    }
    runlog::idf::job_end();
}

extern "C" void app_main(void) {
    runlog::idf::log_capture_start(static_cast<size_t>(CONFIG_DFU32_LOG_RING_KB) * 1024);
    const esp_app_desc_t* app = esp_app_get_description();
    ESP_LOGI(TAG, "DFU32 %s (ESP-IDF %s), board profile '%s'", app->version, app->idf_ver,
             CONFIG_DFU32_BOARD_NAME);

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip %s rev v%d.%d, %d core(s), PSRAM %u bytes", CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100, chip.cores,
             static_cast<unsigned>(esp_psram_get_size()));

    init_nvs();
    // Needs only NVS and the runlog partition, so do it before the self-check:
    // a failing layout must not leave a stale marker for a later boot.
    record_aborted_run();

    bool ok = true;
    ok &= check_data_partition("staging", kSubtypeStaging, kStagingHeaderSize + dfu::kMaxAppSizeS140v6);
    ok &= check_data_partition("runlog", kSubtypeRunlog, kMinRunlogSlots * kRunlogSlotSize);

    if (!ok) {
        ESP_LOGE(TAG, "layout self-check FAILED; refusing to mark this image valid");
        return;
    }
    ESP_LOGI(TAG, "layout self-check OK");

    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed");
        return;
    }
    if (!dfu32::ble::init()) {
        ESP_LOGE(TAG, "BLE init failed; refusing to mark this image valid");
        return;
    }

    // No network yet, so mark valid once the self-check and BLE init pass.
    // M3 moves this after Wi-Fi + HTTP come up (SPEC.md §12.3, rollback).
    esp_ota_mark_app_valid_cancel_rollback();

    dfu32::start_console();
}
