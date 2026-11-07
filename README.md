# DFU32

DFU32 is ESP32 firmware that flashes nRF52 boards over BLE using Nordic **legacy** DFU (the Adafruit nRF52 bootloader and the OTAFIX fork). It pulls firmware from your own server and replaces the phone app, so a remote repeater can be updated without anyone standing next to it.

DFU32 is firmware-agnostic. The motivating use case is MeshCore LoRa repeaters.

**Status:** Milestone M3b. A JSON HTTP API (token claimed on first use) configures the target, stages its image from your server, and arms a flash that waits for `start ota`, hands the radio over to BLE, flashes with automatic retries, brings WiFi back and records the run. A USB-serial console covers the same ground for debugging. The web UI, Improv BLE provisioning and self-OTA come next (M4), then a browser installer on GitHub Pages that flashes DFU32 and sets its WiFi over Improv Serial (M5). See [docs/BENCH.md](docs/BENCH.md) and [docs/MILESTONES.md](docs/MILESTONES.md).

- [SPEC.md](SPEC.md): the full specification
- [docs/MILESTONES.md](docs/MILESTONES.md): phased plan
- [docs/OPEN_QUESTIONS.md](docs/OPEN_QUESTIONS.md): open questions and assumptions
- [docs/PROTOCOL_NOTES.md](docs/PROTOCOL_NOTES.md): verified protocol facts, with source citations
- [docs/BENCH.md](docs/BENCH.md): bench guide for staging, probing and flashing a repeater

## Building the firmware

You need ESP-IDF **v6.1** (for example installed with `eim`). Activate it in your shell, then pick a board profile. Each profile sets the chip target, flash size, partition table, PSRAM, console and pins, so **don't run `idf.py set-target`**. Give each profile its own build directory:

```bash
idf.py -B build/feather_esp32s3 -DDFU32_BOARD=feather_esp32s3 build
```

```bash
idf.py -B build/feather_esp32s3 -p /dev/cu.usbmodem1101 flash monitor
```

Use your own serial port in place of `/dev/cu.usbmodem1101`.

| Profile | Board | Flash / PSRAM |
|---|---|---|
| `feather_esp32s3` (default) | Adafruit Feather ESP32-S3 #5477 | 4 MB / 2 MB |
| `feather_esp32_v2` | Adafruit Feather ESP32 V2 | 8 MB / 2 MB |
| `esp32s3_n16r8` | Generic ESP32-S3 N16R8 DevKitC | 16 MB / 8 MB |

PSRAM is required. A board without it won't boot this firmware.

Change settings for a profile with `idf.py -B build/<profile> menuconfig`. The generated `sdkconfig` lives in that build directory.

Component versions are pinned per chip in `dependencies.lock.esp32` and `dependencies.lock.esp32s3`, and CI fails if a build changes them. After changing an `idf_component.yml`, build one profile for each chip and commit the updated lock files.

Check the app against its size budget (90% of an OTA slot):

```bash
python3 tools/check_app_size.py build/feather_esp32s3
```

## Host tests

The protocol and job logic in `components/dfu_proto`, `components/dfu32_core` and `components/runlog` is plain C++20, so its tests run on your computer. They need CMake ≥ 3.22, Ninja and a C++20 compiler; Catch2 is fetched automatically.

```bash
cmake -S test/host -B build-host -G Ninja && cmake --build build-host && ctest --test-dir build-host --output-on-failure
```

To also check the CRC16 against a real image, point two environment variables at an extracted DFU zip:

```bash
DFU32_TEST_BIN=path/to/firmware.bin DFU32_TEST_DAT=path/to/firmware.dat ctest --test-dir build-host --output-on-failure
```

Test the Python staging tool:

```bash
python3 -m unittest discover -s tools -p 'test_*.py'
```

Check the partition tables:

```bash
python3 tools/check_partitions.py partitions/4mb.csv:4M partitions/8mb.csv:8M
```

## License

MIT
