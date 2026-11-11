# DFU32

DFU32 is ESP32 firmware that flashes nRF52 boards over BLE using Nordic **legacy** DFU (the Adafruit nRF52 bootloader and the OTAFIX fork). It pulls firmware from your own server and replaces the phone app, so a remote repeater can be updated without anyone standing next to it.

DFU32 is firmware-agnostic. The motivating use case is MeshCore LoRa repeaters.

**Status:** Milestone M5. A browser installer on GitHub Pages flashes DFU32 and sets its WiFi over Improv Serial (or join over Improv BLE from Home Assistant). DFU32 then serves a web UI (with an optional API token) that configures the target, stages its image from your server and flashes the repeater over BLE once you run `start ota`, with automatic retries, run records and live progress. It updates itself from the same UI, with rollback. Next is v1.0 hardening (M6): the soak test, the other boards and release builds. Start with [docs/OPERATOR.md](docs/OPERATOR.md); see also [docs/RECOVERY.md](docs/RECOVERY.md), [docs/BENCH.md](docs/BENCH.md) and [docs/MILESTONES.md](docs/MILESTONES.md).

- [SPEC.md](SPEC.md): the full specification
- [docs/MILESTONES.md](docs/MILESTONES.md): phased plan
- [docs/OPEN_QUESTIONS.md](docs/OPEN_QUESTIONS.md): open questions and assumptions
- [docs/PROTOCOL_NOTES.md](docs/PROTOCOL_NOTES.md): verified protocol facts, with source citations
- [docs/BENCH.md](docs/BENCH.md): bench guide for staging, probing and flashing a repeater

## Install from your browser

The installer page at **[dfu32.app](https://dfu32.app/)** flashes DFU32 over USB from a desktop browser with Web Serial (Chrome, Edge, or Firefox 151 or later); no toolchain needed. Pick your board, click **Connect device**, then **Install DFU32**. A device that already runs DFU32 tells the page its board, so you can connect without picking one to update it, change its WiFi or open its page. Updating from the page keeps WiFi, the API token, the target and the staged image, unless you choose "Erase device". The page remembers the board you pick in a cookie, so it's already selected when you come back to update or re-flash ("Forget saved board" under the picker clears it). Until v1.0 the page serves the latest `main` build by default; after that, the latest release.

**Choose a version** lists the earlier releases too (pre-releases included), so you can go back to one. A release whose settings partitions sit elsewhere than in the latest build says it needs an erase. A release published since the page last deployed appears after the next deploy (a push to `main`, or a manual run of CI).

To try a build that isn't released, such as a pull request's, choose **Custom build from a .zip…** and pick the `dfu32-<board>` artifact from that CI run (the run's Summary page, under Artifacts; not the `-debug` one, which holds only the app). The page reads the zip in the browser and installs its separate parts, so settings are kept as with any update. It refuses a build for another board than the connected DFU32 reports, and warns if the build moves `nvs`, `runlog` or `staging`.

To stage a repeater image without an artifact server, connect a DFU32 from the page and choose **Stage nRF52 image**, then pick the DFU `.zip` (a MeshCore release asset, for example). The page checks the package, writes it to DFU32's staging partition over USB and restarts DFU32, keeping its settings; then flash the repeater from DFU32's page with **Flash from: staged image**.

Once DFU32 is on your network, the same works without USB: on DFU32's page, **Image** takes the DFU `.zip` (the browser unpacks and checks it, DFU32 checks it again and stages it), and `python3 tools/dfu32_stage_image.py <zip> --upload dfu32-xxxx.local` does it from a script (`POST /api/v1/staging`). A target then needs only its MAC; a manifest URL is only for having DFU32 fetch images from your own server.

After installing, the page asks for your WiFi network (Improv Serial) and then offers a link to the device's web UI. Coming back later, it recognises an installed DFU32 and can change its WiFi or open its page. WiFi can also be set over Improv BLE (Home Assistant or [improv-wifi.com](https://www.improv-wifi.com/)) or with the `wifi <ssid> <password>` console command.

To flash without the page, each release carries a merged image per board, `dfu32-<board>-merged.bin`. It's a full image for a fresh install: it **resets saved settings** (WiFi, token, target and run log). Flash it at offset 0:

```bash
esptool.py --chip auto -p PORT write_flash 0x0 dfu32-feather_esp32s3-merged.bin
```

To preview the page locally, copy in ESP Web Tools and esptool-js (from hash-pinned npm tarballs; `web/vendor/` is git-ignored) and serve `web/`. Localhost counts as a secure context, so Web Serial works; the manifests only exist on the deployed site.

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
| `rak11200_rak19007` | RAKwireless WisBlock RAK11200 on a RAK19007 base | 4 MB / 8 MB (4 MB mapped) |
| `xiao_esp32s3` | Seeed Studio XIAO ESP32S3 (or XIAO ESP32S3 Sense) | 8 MB / 8 MB |
| `xiao_esp32c5` | Seeed Studio XIAO ESP32C5 | 8 MB / 8 MB |

PSRAM is required. A board without it won't boot this firmware.

The RAK11200 has no auto-reset circuit, so `idf.py flash` can't put it into download mode by itself. Pull BOOT0 to GND, press reset, flash, then release BOOT0 and press reset again. Its status LED is the blue one on GPIO2 (the RAK19007 has no NeoPixel). It has no user button, so a lost API token is cleared with `token clear` on the USB console (or by reflashing with an NVS erase); while you still have the token, `POST /api/v1/system/token/clear` turns it off and `/rotate` replaces it.

The XIAO ESP32S3 has no on-board antenna, so fit the U.FL antenna it ships with before using WiFi or BLE. Its attention LED is the orange user LED (GPIO21); it has no NeoPixel. The BOOT button is the user button.

The XIAO ESP32C5 (ESP32-C5, RISC-V) is the same: fit its U.FL antenna, the attention LED is the yellow user LED (GPIO27), and BOOT (GPIO28) is the user button. It can join 5 GHz networks as well as 2.4 GHz. Its bootloader sits at `0x2000`, which the builds, the merged image and the installer page already account for.

Change settings for a profile with `idf.py --preset <profile> menuconfig`. The generated `sdkconfig` lives in that build directory.

To add a board, create `boards/<name>/sdkconfig.defaults` and a matching preset in `CMakePresets.json` (`binaryDir` `build/<name>`, `DFU32_BOARD` `<name>`). `python3 tools/check_presets.py` checks they agree, and CI builds every preset. Personal presets go in a `CMakeUserPresets.json`, which git ignores.

Component versions are pinned per chip in `dependencies.lock.esp32`, `dependencies.lock.esp32s3` and `dependencies.lock.esp32c5`, and CI fails if a build changes them. After changing an `idf_component.yml`, build one profile for each chip and commit the updated lock files.

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

Test the Python tools (staging, packing, the MeshCore release fetcher and the bench runner) and the installer page's staging code (`web/staging.js`, run under Node 20 or later and checked against the Python tool; skipped without Node):

```bash
python3 -m unittest discover -s tools -p 'test_*.py'
```

Check the partition tables:

```bash
python3 tools/check_partitions.py partitions/4mb.csv:4M partitions/8mb.csv:8M
```

## Bench runner

With DFU32 and a repeater on your desk, `tools/dfu32_bench.py` runs the bench checklist automatically through the serial console and the HTTP API. It stops only when you need to send `start ota`. `--soak 20` runs the M6 soak: 20 flashes alternating two versions, with faults mixed in. See [docs/BENCH.md §7](docs/BENCH.md#7-automated-bench-run).

To get a MeshCore release ready to serve, `tools/dfu32_meshcore_fetch.py --tag repeater-v1.18.0 --env RAK_4631_repeater --board rak4631 --out site/rak4631` finds the right asset on GitHub, downloads it and packs it (see [docs/BENCH.md §4](docs/BENCH.md#4-stage-an-image)).

## License

MIT

The firmware and the installer page include third-party software under its own licenses (Apache-2.0, MIT, BSD); see [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES), generated by `tools/dfu32_notices.py`.
