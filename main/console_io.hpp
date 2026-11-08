// SPDX-License-Identifier: MIT
//
// The console port's input, shared by the REPL and Improv Serial (SPEC.md
// §9.2). One reader task takes every byte from the driver and runs it through
// the Improv framer: frames go to Improv Serial, everything else to the line
// editor. Nothing else may read the port (stdin) directly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/types.h>

#include "freertos/FreeRTOS.h"

namespace dfu32::console_io {

/// Installs the console driver and starts the reader and Improv Serial. Call
/// early in boot (the installer expects an answer within 10 s), after
/// net::init().
bool start();

/// Console input for the line editor, with CR turned into LF as the stock
/// console does. Waits up to `wait` for the first byte; returns the count
/// (0 on timeout).
size_t read(void* buf, size_t n, TickType_t wait);

/// linenoise's read function (linenoiseSetReadFunction): blocks for input.
ssize_t linenoise_read(int fd, void* buf, size_t n);

/// Sends an Improv frame on a line of its own, byte for byte (the console
/// otherwise turns '\n' into "\r\n"), in one write so log output can't land
/// inside it.
void write_frame(std::span<const uint8_t> frame);

}  // namespace dfu32::console_io
