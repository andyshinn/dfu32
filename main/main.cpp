// SPDX-License-Identifier: MIT
//
// DFU32: boot banner, layout self-check, run-log recovery, NimBLE central,
// WiFi (net), the job orchestrator, the HTTP API and the USB-serial bench
// console.
#include <cinttypes>
#include <cstdint>
#include <cstring>

#include "api/api.hpp"
#include "config/config.hpp"
#include "console.hpp"
#include "console_io.hpp"
#include "dfu_ble/dfu_ble.hpp"
#include "dfu_proto/image.hpp"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "improv/improv.hpp"
#include "indicator/indicator.hpp"
#include "job/job.hpp"
#include "net/net.hpp"
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

// ---- self-OTA rollback (SPEC.md §10.1, §12.3) ---------------------------------------
// After POST /api/v1/system/ota the bootloader starts the new image as
// "pending verify". A failed self-check, or an HTTP API that doesn't start,
// rolls back at once. WiFi is then the deciding signal, but only for faults
// the new image could have caused:
//  - an address within 90 s: confirmed;
//  - no network stored: confirmed (there is nothing to test against);
//  - the radio is demonstrably working but the network isn't reachable (the
//    driver reported a disconnect: AP not found, wrong passphrase, ...):
//    confirmed with a warning, so a router that is down during the update
//    doesn't throw away a good image;
//  - no sign of life from WiFi at all: rolled back.

namespace {

constexpr uint32_t kOtaConfirmMs = 90000;

bool ota_pending_verify() {
    esp_ota_img_states_t st{};
    const esp_partition_t* running = esp_ota_get_running_partition();
    return running != nullptr && esp_ota_get_state_partition(running, &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

/// A boot step failed. A just-installed image goes back to the previous one;
/// otherwise there is nothing to roll back to, so keep running degraded.
void boot_failed(const char* why) {
    ESP_LOGE(TAG, "%s", why);
    if (ota_pending_verify()) {
        ESP_LOGE(TAG, "new firmware failed its first boot: rolling back to the previous image");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void ota_confirm_task(void*) {
    const bool up = dfu32::net::status().provisioned && dfu32::net::wait_connected(kOtaConfirmMs);
    const dfu32::net::Status ns = dfu32::net::status();
    if (up) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed: WiFi and the HTTP API are up");
    } else if (!ns.provisioned) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGW(TAG, "new firmware confirmed without a network test: no WiFi network is stored");
    } else if (ns.last_disconnect_reason != 0) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGW(TAG, "new firmware confirmed: WiFi works but \"%s\" isn't reachable (reason %u); not the "
                      "firmware's fault",
                 ns.ssid.c_str(), ns.last_disconnect_reason);
    } else {
        ESP_LOGE(TAG, "new firmware shows no WiFi activity after %u s: rolling back to the previous image",
                 static_cast<unsigned>(kOtaConfirmMs / 1000));
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    vTaskDelete(nullptr);
}

}  // namespace

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
        boot_failed("layout self-check FAILED; refusing to mark this image valid");
        return;
    }
    ESP_LOGI(TAG, "layout self-check OK");

    if (psa_crypto_init() != PSA_SUCCESS) {
        boot_failed("psa_crypto_init failed");
        return;
    }
    if (!dfu32::ble::init()) {
        boot_failed("BLE init failed; refusing to mark this image valid");
        return;
    }

    // WiFi failing to start is logged but not fatal: the console, staging
    // from a PC and flashing still work without it.
    const bool net_ok = dfu32::net::init();
    if (!net_ok) ESP_LOGE(TAG, "WiFi init failed; networking disabled");

    if (!dfu32::job::init()) {
        boot_failed("job orchestrator failed to start; refusing to mark this image valid");
        return;
    }
    // The API needs the network stack; without it the console still works.
    const bool api_ok = net_ok && dfu32::api::start();
    if (net_ok && !api_ok) ESP_LOGE(TAG, "HTTP API failed to start");

    // Improv BLE (SPEC.md §9.2): open while no network is stored; the button
    // (or `improv start`) opens it later.
    if (net_ok && !dfu32::improv::init()) ESP_LOGE(TAG, "Improv BLE init failed");
    if (net_ok && !dfu32::net::status().provisioned) {
        ESP_LOGI(TAG, "no WiFi network stored: set one from the installer page (Improv Serial), over Improv BLE, "
                      "or with `wifi <ssid> <password>`");
        dfu32::improv::open(dfu32::improv::Window::Unprovisioned);
    }
    // The console reader, and Improv Serial with it, as early as the network
    // allows: the installer page asks within seconds of resetting the board.
    if (!dfu32::console_io::start()) ESP_LOGE(TAG, "console input failed to start");

    dfu32::indicator::Hooks hooks;
    hooks.improv_active = [] { return dfu32::improv::active(); };
    hooks.identifying = [] { return dfu32::improv::identifying(); };
    hooks.on_improv_window = [] {
        if (!dfu32::improv::open(dfu32::improv::Window::Button)) {
            ESP_LOGW(TAG, "Improv BLE not opened (disabled, or BLE busy with a job)");
        }
    };
    hooks.on_unclaim = [] {
        ESP_LOGW(TAG, "%s", dfu32::api::unclaim() ? "API token cleared: the API is open until a token is set again"
                                                    : "clearing the API token failed");
    };
    hooks.on_factory_reset = [] {
        // SPEC.md §9.3: settings and WiFi go; the staged image and run log stay.
        // Never in the middle of a flash (it would leave the target erased) or
        // a console/API operation: those hold the BLE lease. Improv holding it
        // is fine, the reboot ends it.
        const char* holder = dfu32::job::holder();
        const bool improv = holder != nullptr && std::strcmp(holder, "improv") == 0;
        if (holder != nullptr && !improv) {
            ESP_LOGE(TAG, "factory reset refused: %s is using BLE/staging; abort it (`job abort`) and hold again",
                     holder);
            return;
        }
        // Take the lease so no job can start while erasing.
        if (!improv && !dfu32::job::try_acquire("reset")) {
            ESP_LOGE(TAG, "factory reset refused: something just started; try again");
            return;
        }
        if (!dfu32::config::erase_all()) {
            ESP_LOGE(TAG, "factory reset failed: could not erase the settings; nothing else changed, not rebooting");
            if (!improv) dfu32::job::release();
            return;
        }
        if (!dfu32::net::forget()) {
            ESP_LOGE(TAG, "factory reset incomplete: settings erased, but the WiFi credentials could not be "
                          "cleared (`wifi forget`); not rebooting");
            if (!improv) dfu32::job::release();
            return;
        }
        ESP_LOGW(TAG, "factory reset: settings erased, WiFi forgotten; rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    };
    dfu32::indicator::start(hooks);

    if (ota_pending_verify()) {
        if (!api_ok) {
            boot_failed("new firmware can't start WiFi or the HTTP API");
        } else {
            ESP_LOGW(TAG, "new firmware: confirming once WiFi is up (up to %u s)",
                     static_cast<unsigned>(kOtaConfirmMs / 1000));
            xTaskCreate(&ota_confirm_task, "ota_confirm", 3072, nullptr, 3, nullptr);
        }
    } else {
        esp_ota_mark_app_valid_cancel_rollback();  // normal boot: nothing pending
    }

    dfu32::start_console();
}
