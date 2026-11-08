# DFU32

DFU32 is ESP32 firmware that flashes nRF52 boards over BLE using Nordic **legacy** DFU (the Adafruit nRF52 bootloader and the OTAFIX fork). It pulls firmware from your own server and replaces the phone app, so a remote repeater can be updated without anyone standing next to it.

DFU32 is firmware-agnostic. The motivating use case is MeshCore LoRa repeaters.

**Status:** Milestone M4a. A JSON HTTP API (token claimed on first use) configures the target, stages its image from your server, and arms a flash that waits for `start ota`, hands the radio over to BLE, flashes with automatic retries, brings WiFi back and records the run. WiFi is set over Improv BLE (from Home Assistant or the Improv web page) or the USB console, and the NeoPixel, red LED and button follow SPEC §9.3/§13.4. The web UI and self-OTA come next (M4b), then a browser installer on GitHub Pages that flashes DFU32 and sets its WiFi over Improv Serial (M5). See [docs/BENCH.md](docs/BENCH.md) and [docs/MILESTONES.md](docs/MILESTONES.md).

- [SPEC.md](SPEC.md): the full specification
- [docs/MILESTONES.md](docs/MILESTONES.md): phased plan
- [docs/OPEN_QUESTIONS.md](docs/OPEN_QUESTIONS.md): open questions and assumptions
- [docs/PROTOCOL_NOTES.md](docs/PROTOCOL_NOTES.md): verified protocol facts, with source citations
- [docs/BENCH.md](docs/BENCH.md): bench guide for staging, probing and flashing a repeater

## Install from your browser

The installer page at **[andyshinn.github.io/dfu32](https://andyshinn.github.io/dfu32/)** flashes DFU32 over USB from a desktop browser with Web Serial (Chrome, Edge, or Firefox 151 or later); no toolchain needed. Pick your board and click Install. Updating from the page keeps WiFi, the API token, the target and the staged image, unless you choose "Erase device". Until v1.0 the page serves the latest `main` build; after that, the latest release.

WiFi setup from the page itself (Improv Serial) arrives later in M5. Until then, after installing, set WiFi over Improv BLE (Home Assistant or [improv-wifi.com](https://www.improv-wifi.com/)) or with the `wifi <ssid> <password>` console command.

To flash without the page, each release carries a merged image per board, `dfu32-<board>-merged.bin`. It's a full image for a fresh install: it **resets saved settings** (WiFi, token, target and run log). Flash it at offset 0:

```bash
esptool.py --chip auto -p PORT write_flash 0x0 dfu32-feather_esp32s3-merged.bin
```

To preview the page locally, copy in ESP Web Tools (from a hash-pinned npm tarball; `web/vendor/` is git-ignored) and serve `web/`. Localhost counts as a secure context, so Web Serial works; the manifests only exist on the deployed site.

```bash
python3 tools/fetch_esp_web_tools.py --site web && python3 -m http.server -d web 8000
```

## Building the firmware

You need ESP-IDF **v6.1** (for example installed with `eim`). Activate it in your shell, then pick a board profile with `--preset`. Each profile sets the chip target, flash size, partition table, PSRAM, console and pins, so **don't run `idf.py set-target`**. Each preset (see [CMakePresets.json](CMakePresets.json)) builds in its own directory, `build/<profile>`:

```bash
idf.py --preset feather_esp32s3 build
```

```bash
idf.py --preset feather_esp32s3 -p /dev/cu.usbmodem1101 flash monitor
```

Use your own serial port in place of `/dev/cu.usbmodem1101`.

If you work on one board for a while, set `IDF_PRESET` once and drop `--preset`:

```bash
export IDF_PRESET=feather_esp32s3
```

```bash
idf.py build flash monitor
```

Without `--preset` or `IDF_PRESET`, `idf.py` uses the first preset (`feather_esp32s3`) and prints a notice saying so. Always pass `--preset`, even when you add `-B` for a separate directory such as the bench debug build (`idf.py --preset <profile> -B build/<profile>-debug -DDFU32_DEBUG_ENDPOINTS=ON build`): with presets present, `idf.py -B <dir>` on its own applies that default preset when it configures.

To build every profile in turn:

```bash
python3 tools/build_all.py
```

The script's own options (`--only <profile>`, repeatable, limits the run) come first. Everything after them goes to `idf.py` in place of `build`, with `{preset}` replaced by the profile name, for example `python3 tools/build_all.py build merge-bin -o dfu32-{preset}-merged.bin`. Put `--` before `idf.py` arguments that start with a dash: `python3 tools/build_all.py --only esp32s3_n16r8 -- -p PORT flash`. A failed board doesn't stop the rest; the summary at the end lists each one.

Build directories from the older `idf.py -B build/<profile> -DDFU32_BOARD=<profile>` commands are the same directories the presets use, so they keep working with `--preset`. If a build directory named after one profile ends up configured for another, CMake stops with an error telling you which `--preset` to use.

| Profile | Board | Flash / PSRAM |
|---|---|---|
| `feather_esp32s3` (first preset) | Adafruit Feather ESP32-S3 #5477 | 4 MB / 2 MB |
| `feather_esp32_v2` | Adafruit Feather ESP32 V2 | 8 MB / 2 MB |
| `esp32s3_n16r8` | Generic ESP32-S3 N16R8 DevKitC | 16 MB / 8 MB |

PSRAM is required. A board without it won't boot this firmware.

Change settings for a profile with `idf.py --preset <profile> menuconfig`. The generated `sdkconfig` lives in that build directory.

To add a board, create `boards/<name>/sdkconfig.defaults` and a matching preset in `CMakePresets.json` (`binaryDir` `build/<name>`, `DFU32_BOARD` `<name>`). `python3 tools/check_presets.py` checks they agree, and CI builds every preset. Personal presets go in a `CMakeUserPresets.json`, which git ignores.

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

Test the Python tools (staging, packing and the bench runner):

```bash
python3 -m unittest discover -s tools -p 'test_*.py'
```

Check the partition tables:

```bash
python3 tools/check_partitions.py partitions/4mb.csv:4M partitions/8mb.csv:8M
```

## Bench runner

With DFU32 and a repeater on your desk, `tools/dfu32_bench.py` runs the bench checklist automatically through the serial console and the HTTP API. It stops only when you need to send `start ota`. See [docs/BENCH.md §7](docs/BENCH.md#7-automated-bench-run).

## License

MIT
