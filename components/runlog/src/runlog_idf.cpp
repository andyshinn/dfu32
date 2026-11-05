// SPDX-License-Identifier: MIT
#include "runlog/runlog_idf.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "runlog/log_ring.hpp"

namespace runlog::idf {
namespace {

constexpr const char* TAG = "runlog";
constexpr auto kSubtypeRunlog = static_cast<esp_partition_subtype_t>(0x41);
constexpr const char* kNvsNamespace = "dfu32";
constexpr const char* kJobKey = "job_active";
constexpr uint32_t kJobMagic = 0x4A4F4231;  // "JOB1"

// ---- log capture ----

LogRing* s_ring = nullptr;
SemaphoreHandle_t s_ring_mutex = nullptr;
vprintf_like_t s_prev_vprintf = nullptr;

char s_line[256];  // formatting buffer, guarded by s_ring_mutex (keeps it off small task stacks)

int log_hook(const char* fmt, va_list ap) {
    // ISR context can't take a mutex; such output only goes to the console.
    // Otherwise never block a logging task for long: if a reader holds the
    // ring, skip the capture (not the console output).
    if (s_ring != nullptr && !xPortInIsrContext() && xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        va_list copy;
        va_copy(copy, ap);
        const int n = std::vsnprintf(s_line, sizeof(s_line), fmt, copy);
        va_end(copy);
        if (n > 0) {
            if (static_cast<size_t>(n) >= sizeof(s_line)) {
                // Truncated: keep the line structure intact for since().
                s_line[sizeof(s_line) - 2] = '\n';
                s_ring->append({s_line, sizeof(s_line) - 1});
            } else {
                s_ring->append({s_line, static_cast<size_t>(n)});
            }
        }
        xSemaphoreGive(s_ring_mutex);
    }
    return s_prev_vprintf != nullptr ? s_prev_vprintf(fmt, ap) : std::vprintf(fmt, ap);
}

// ---- partition storage ----

class PartitionStorage final : public ISlotStorage {
public:
    explicit PartitionStorage(const esp_partition_t* p) : part_(p) {}
    size_t slot_count() const override { return part_->size / kSlotSize; }
    bool read_slot(size_t index, std::span<uint8_t, kSlotSize> out) override {
        return esp_partition_read(part_, index * kSlotSize, out.data(), kSlotSize) == ESP_OK;
    }
    bool write_slot(size_t index, std::span<const uint8_t, kSlotSize> data) override {
        const size_t off = index * kSlotSize;
        return esp_partition_erase_range(part_, off, kSlotSize) == ESP_OK &&
               esp_partition_write(part_, off, data.data(), kSlotSize) == ESP_OK;
    }

private:
    const esp_partition_t* part_;
};

// ---- job marker ----

struct StoredJob {
    uint32_t magic;
    JobMarker marker;
};

bool save_job(const JobMarker& m) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    const StoredJob stored{kJobMagic, m};
    const bool ok = nvs_set_blob(h, kJobKey, &stored, sizeof(stored)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

JobMarker s_current{};

}  // namespace

bool log_capture_start(size_t bytes) {
    if (s_ring != nullptr) return true;
    char* mem = static_cast<char*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (mem == nullptr) {
        bytes = std::min<size_t>(bytes, 8 * 1024);
        mem = static_cast<char*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    }
    s_ring_mutex = xSemaphoreCreateMutex();
    if (mem == nullptr || s_ring_mutex == nullptr) {
        ESP_LOGE(TAG, "log capture: out of memory");
        return false;
    }
    s_ring = new LogRing({mem, bytes});
    s_prev_vprintf = esp_log_set_vprintf(&log_hook);
    ESP_LOGI(TAG, "capturing logs in a %u KB ring", static_cast<unsigned>(bytes / 1024));
    return true;
}

uint64_t log_position() {
    if (s_ring == nullptr) return 0;
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    const uint64_t p = s_ring->position();
    xSemaphoreGive(s_ring_mutex);
    return p;
}

std::string log_since(uint64_t from, size_t max_bytes) {
    if (s_ring == nullptr) return {};
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    std::string s = s_ring->since(from, max_bytes);
    xSemaphoreGive(s_ring_mutex);
    return s;
}

std::string log_tail(size_t max_bytes) { return log_since(0, max_bytes); }

std::string log_read(uint64_t from, size_t max_bytes, uint64_t& end) {
    end = 0;
    if (s_ring == nullptr) return {};
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    end = s_ring->position();
    std::string s = s_ring->since(from, max_bytes);
    xSemaphoreGive(s_ring_mutex);
    return s;
}

RunLog* open_run_log() {
    static PartitionStorage* storage = nullptr;
    static RunLog* log = nullptr;
    if (log != nullptr) return log;
    const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kSubtypeRunlog, "runlog");
    if (p == nullptr) {
        ESP_LOGE(TAG, "no 'runlog' partition; run records disabled");
        return nullptr;
    }
    storage = new PartitionStorage(p);
    log = new RunLog(*storage);
    log->scan();
    ESP_LOGI(TAG, "run log: %u slots, next run #%u", static_cast<unsigned>(storage->slot_count()),
             static_cast<unsigned>(log->next_seq()));
    return log;
}

bool job_begin(const JobMarker& m) {
    s_current = m;
    return save_job(s_current);
}

bool job_set_stage(JobStage stage) {
    if (s_current.stage == stage) return true;
    s_current.stage = stage;
    return save_job(s_current);
}

bool job_end() {
    s_current = JobMarker{};
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    const esp_err_t err = nvs_erase_key(h, kJobKey);
    const bool ok = (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool job_pending(JobMarker& out) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    StoredJob stored{};
    size_t len = sizeof(stored);
    const bool ok = nvs_get_blob(h, kJobKey, &stored, &len) == ESP_OK && len == sizeof(stored) &&
                    stored.magic == kJobMagic;
    nvs_close(h);
    if (ok) {
        out = stored.marker;
        out.target[sizeof(out.target) - 1] = '\0';
        out.version[sizeof(out.version) - 1] = '\0';
    }
    return ok;
}

const char* reset_reason_string() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "UNKNOWN";
    }
}

}  // namespace runlog::idf
