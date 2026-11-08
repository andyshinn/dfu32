// SPDX-License-Identifier: MIT
#include "console_io.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <unistd.h>
#include <vector>

#include "esp_log.h"
#include "esp_stdio.h"
#include "esp_timer.h"
#include "esp_vfs_common.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "improv/improv.hpp"
#include "improv/serial_frame.hpp"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#elif CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

namespace dfu32::console_io {
namespace {

constexpr const char* TAG = "console";
constexpr size_t kLineBufferSize = 512;  // typed input waiting for the line editor
// The console's output translation as esp_stdio_install_io_driver() sets it
// (the VFS has no getter); write_frame() turns it off and back to this.
constexpr esp_line_endings_t kTextLineEndings = ESP_LINE_ENDINGS_CRLF;

StreamBufferHandle_t g_line = nullptr;
std::mutex g_write_mu;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

/// Raw bytes from the driver (no line-ending translation): whatever has
/// arrived, waiting up to `ticks` for the first byte.
int raw_read(uint8_t* buf, size_t n, TickType_t ticks) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(buf, n, ticks);
#elif CONFIG_ESP_CONSOLE_UART
    // uart_read_bytes waits for all `n` bytes: wait for one, then take the
    // rest that is already buffered.
    const int first = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buf, 1, ticks);
    if (first <= 0) return first;
    size_t avail = 0;
    uart_get_buffered_data_len(CONFIG_ESP_CONSOLE_UART_NUM, &avail);
    avail = std::min(avail, n - 1);
    const int more = avail > 0 ? uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buf + 1, avail, 0) : 0;
    return 1 + std::max(more, 0);
#else
    (void)buf;
    (void)n;
    vTaskDelay(ticks);
    return 0;
#endif
}

/// Output translation: '\n' -> "\r\n" for text, off for a frame.
void raw_output(bool raw) {
    const esp_line_endings_t mode = raw ? ESP_LINE_ENDINGS_LF : kTextLineEndings;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_vfs_set_tx_line_endings(mode);
#elif CONFIG_ESP_CONSOLE_UART
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, mode);
#else
    (void)mode;
#endif
}

void to_line_editor(std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return;
    // What the stock console's input translation did: terminals send CR for
    // Enter, linenoise wants LF.
    std::replace(bytes.begin(), bytes.end(), static_cast<uint8_t>('\r'), static_cast<uint8_t>('\n'));
    // Never block the reader (it must keep serving Improv): while a command
    // runs and nobody reads, extra typing is dropped, as it was before.
    xStreamBufferSend(g_line, bytes.data(), bytes.size(), 0);
    bytes.clear();
}

void on_event(improv::SerialFramer::Event e, const improv::SerialFramer& f) {
    using Event = improv::SerialFramer::Event;
    switch (e) {
        case Event::None: return;
        case Event::Frame: improv::serial_deliver(f.type(), f.data()); return;
        case Event::BadChecksum:
            ESP_LOGW(TAG, "dropped an Improv frame with a bad checksum");
            improv::serial_invalid_frame();
            return;
        case Event::TooLong:
            ESP_LOGW(TAG, "dropped an oversized Improv frame");
            improv::serial_invalid_frame();
            return;
        // No reply to these two: a client speaking another protocol version
        // couldn't read our (version 1) error frame either, and a cut-off
        // frame's sender is gone or will resend it.
        case Event::BadVersion: ESP_LOGW(TAG, "dropped an Improv frame of an unknown version"); return;
        case Event::TimedOut: ESP_LOGW(TAG, "dropped a cut-off Improv frame"); return;
    }
}

void reader_task(void*) {
    improv::SerialFramer framer;
    std::vector<uint8_t> line;
    line.reserve(64);
    uint8_t rx[64];
    for (;;) {
        const uint32_t wait = framer.wait_ms(now_ms());
        const TickType_t ticks =
            wait == improv::SerialFramer::kNoDeadline ? portMAX_DELAY : pdMS_TO_TICKS(wait) + 1;
        const int n = raw_read(rx, sizeof(rx), ticks);
        const uint32_t now = now_ms();
        if (n <= 0) {
            on_event(framer.poll(now, line), framer);
        } else {
            for (int i = 0; i < n; ++i) {
                on_event(framer.push(rx[i], now, line), framer);
            }
            improv::wipe(std::span<uint8_t>(rx, sizeof(rx)));  // may have held a passphrase
        }
        to_line_editor(line);
    }
}

}  // namespace

bool start() {
    if (g_line != nullptr) return true;
    std::fflush(stdout);
    fsync(fileno(stdout));
    if (esp_stdio_install_io_driver() != ESP_OK) {
        ESP_LOGE(TAG, "console driver install failed");
        return false;
    }
    g_line = xStreamBufferCreate(kLineBufferSize, 1);
    if (g_line == nullptr) return false;
    if (!improv::serial_start(&write_frame)) ESP_LOGE(TAG, "Improv Serial failed to start");
    if (xTaskCreate(&reader_task, "console_rx", 4096, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "console reader failed to start");
        return false;
    }
    return true;
}

size_t read(void* buf, size_t n, TickType_t wait) {
    if (g_line == nullptr || n == 0) {
        vTaskDelay(wait);  // no reader (start() failed): never spin
        return 0;
    }
    return xStreamBufferReceive(g_line, buf, n, wait);
}

ssize_t linenoise_read(int, void* buf, size_t n) {
    for (;;) {
        const size_t got = read(buf, n, portMAX_DELAY);
        if (got > 0) return static_cast<ssize_t>(got);
    }
}

void write_frame(std::span<const uint8_t> frame) {
    std::vector<uint8_t> out;
    out.reserve(frame.size() + 2);
    out.push_back('\n');  // the client only looks for frames at the start of a line
    out.insert(out.end(), frame.begin(), frame.end());
    out.push_back('\n');
    std::lock_guard<std::mutex> lk(g_write_mu);
    std::fflush(stdout);  // pending text goes first, still translated
    // The driver holds its own write lock for the whole call, so no other
    // output can land inside the frame. Text written by another task during
    // these few bytes may lose a '\r'; the frame itself can't be corrupted.
    raw_output(true);
    const int fd = fileno(stdout);
    const ssize_t written = ::write(fd, out.data(), out.size());
    raw_output(false);
    fsync(fd);
    if (written != static_cast<ssize_t>(out.size())) {
        // USB not connected (nobody to read it) or the driver gave up: the
        // frame is dropped and the client will time out.
        ESP_LOGD(TAG, "Improv frame not sent (%d of %u bytes)", static_cast<int>(written),
                 static_cast<unsigned>(out.size()));
    }
}

}  // namespace dfu32::console_io
