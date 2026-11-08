# Bench guide (M1–M4b)

This guide covers bench-testing on your desk: build DFU32, stage an image (over WiFi since M3a, or from the PC with esptool), then probe and flash an nRF52 repeater over BLE.

**You need:**
- a Feather ESP32-S3 (or another supported board) on USB;
- the repeater on USB to your computer, for `start ota` and `ver`;
- the MeshCore DFU `.zip` for that repeater.

Ideally the repeater is a sacrificial board on the OTAFIX bootloader. `flash` erases its app, and an OTAFIX board recovers over BLE if a flash fails.

## 1. Build and flash DFU32

Activate your ESP-IDF 6.1 environment first. Then build:

```bash
idf.py --preset feather_esp32s3 build
```

Then flash and open the monitor (use your own port):

```bash
idf.py --preset feather_esp32s3 -p /dev/cu.usbmodem1101 flash monitor
```

The monitor shows the boot banner, "layout self-check OK", the NimBLE address, and a `dfu32>` prompt. Type `help` to list the commands.

### Or flash a CI build from macOS

Each CI run uploads a `dfu32-<board>` artifact. Its `dfu32-<board>-merged.bin` contains the bootloader, partition table, OTA data and app as one image, written at offset 0. Use esptool, from your activated IDF environment (`python -m esptool`) or a standalone install (`brew install esptool` or `pipx install esptool`).

1. Find the port: `ls /dev/cu.usbmodem*` (Feather S3, native USB) or `ls /dev/cu.usbserial* /dev/cu.wchusbserial*` (Feather V2, USB-UART bridge).
2. The first time only, erase the factory firmware (Adafruit ships TinyUF2/CircuitPython). This also clears NVS and staging:

   ```bash
   python -m esptool --port /dev/cu.usbmodem1101 erase-flash
   ```

3. Write the merged image:

   ```bash
   python -m esptool --port /dev/cu.usbmodem1101 --baud 460800 write-flash 0x0 dfu32-feather_esp32s3-merged.bin
   ```

   The merged image is contiguous from `0x0` to the end of the app, and the gaps between segments are filled with `0xFF`. **That includes NVS, so it erases your stored `target`.** The staging partition lies beyond the app, so the staged image survives.

**Updating DFU32 later:** write only the app, at its offset. NVS (target) and staging are kept:

| Layout | Boards | App offset |
|---|---|---|
| 4 MB | Feather S3 | `0x20000` |
| 8 MB | Feather V2, N16R8 | `0x30000` |

```bash
python -m esptool --port /dev/cu.usbserial-575E0522651 --baud 460800 write-flash 0x30000 build/feather_esp32_v2/dfu32.bin
```

If flashing fails right after "Changing baud rate", the USB-serial link can't sustain that speed. This is common on the Feather V2 with the macOS built-in driver (`/dev/cu.usbserial-*`). Retry with `--baud 115200`.

If esptool can't connect at all, the board isn't in download mode: hold **BOOT**, tap **RESET**, release BOOT, then retry. This is normal while the factory TinyUF2 firmware is still on the Feather S3. Tap RESET after flashing if the board doesn't restart on its own.

Open a serial terminal afterwards to get the `dfu32>` prompt, e.g. `idf.py -p PORT monitor` or `screen /dev/cu.usbmodem1101 115200`.

## 2. Find the repeater's MAC

On the repeater's serial console, or remotely as admin over LoRa, run:

```
start ota
```

It replies `OK - mac: XX:XX:XX:XX:XX:XX` and advertises as e.g. `RAK4631_OTA`. Store that MAC on DFU32:

```
dfu32> target XX:XX:XX:XX:XX:XX
```

To pick the repeater from a list instead, run `scan` and look for the `*_OTA` name.

## 3. Probe: the frame-size test (non-destructive)

With the repeater still in `start ota` mode:

```
dfu32> probe
```

`probe` connects and does the buttonless jump into the bootloader. It reports the bootloader's details:
- DIS firmware revision and model;
- bootloader class;
- ATT MTU and the packet size DFU32 will use;
- PHY.

Then it sends SYS_RESET, so the repeater reboots into its normal app. START is never sent and nothing is erased.

What to look for:
- **packet size 244**: the buttonless path keeps MTU 247 (the fast path; open question 1 answered).
- **packet size 20**: either the MTU stayed at 23, or the bootloader wasn't recognised. The "DIS firmware rev" line tells you which.
- The **DIS firmware rev** string, e.g. `0.9.2-OTAFIX2.2-BP1.3 s140 6.1.1`, confirms the bootloader classifier (open question 7).

Please paste the `PROBE OK` block back so the spec defaults can be updated.

Useful options:
- `probe --no-reset` leaves the repeater in its bootloader, for example to rerun `probe` against the bootloader directly. Run `blreset` afterwards, or power-cycle it.
- `probe --wait 120` scans for longer.

If the repeater doesn't come back after `probe`, power-cycle it. Nothing was erased.

## 4. Stage an image

### Over WiFi (M3a)

Package the release on your computer and serve it over plain HTTP:

```bash
python3 tools/dfu32_pack.py RAK_4631_repeater-v1.18.0-81dbae5.zip --board rak4631 --out site/rak4631
```

Or let `dfu32_meshcore_fetch.py` find and download the MeshCore release for you, then pack it the same way (`--latest repeater` instead of `--tag` takes the newest release; `--list` shows the nRF52 env names):

```bash
python3 tools/dfu32_meshcore_fetch.py --tag repeater-v1.18.0 --env RAK_4631_repeater --board rak4631 --out site/rak4631
```

```bash
python3 -m http.server -d site 8000
```

In the DFU32 monitor, join your WiFi network once (it's remembered across reboots, and it survives an app-only update but not a merged-bin flash), then tell the target where its manifest lives and which board it is:

```
dfu32> wifi MyNetwork MyPassword
dfu32> target board rak4631
dfu32> target url http://<your-computer-ip>:8000/rak4631/manifest.json
dfu32> check
dfu32> stage
dfu32> staged
```

- `wifi` on its own shows the IP, RSSI and the `dfu32-xxxx.local` name.
- `check` fetches the manifest and compares it with the target's board and the staged image. It writes nothing.
- `stage` downloads, verifies and stages the image. Run it again and it notices that the same image is already staged, re-verifies it and skips the download (`stage --force` downloads anyway).
- The `--board` you pack with must match `target board` (case-insensitive), or `stage` stops with `BOARD_MISMATCH` before anything is erased.

Worth trying on the bench:
- **Wrong board:** `target board heltec_t114`, then `stage`. Expect `BOARD_MISMATCH`, and `staged` still shows the old image.
- **Bad hash:** edit one character of `bin.sha256` in `manifest.json`, then `stage --force`. Expect `HASH_MISMATCH`. The old image is gone at this point, because the download had already started.
- **Wrong size:** append a byte to `firmware.bin`. Expect `SIZE_MISMATCH` before anything is erased, because the server's `Content-Length` disagrees with the manifest.
- **404:** point `target url` at a missing file. Expect `HTTP_ERROR: HTTP 404`.
- **WiFi drop:** turn the access point off and on again. `wifi` should show it reconnecting, with the backoff capped at 30 s.

### From the PC with esptool

On your computer, build the staging image:

```bash
python3 tools/dfu32_stage_image.py RAK_4631_repeater-v1.18.0-81dbae5.zip --board rak4631 -o staging.img
```

The tool validates the image and prints the flash command. Quit the monitor (`Ctrl-]`) so the port is free, then write the image to the staging partition. For the 4 MB layout:

```bash
python -m esptool --port /dev/cu.usbmodem1101 write-flash 0x300000 staging.img
```

On the 8 MB layout (Feather V2, N16R8) the offset is `0x630000`.

Back in the monitor, check the image:

```
dfu32> staged
```

It should end with `result OK`.

## 5. Flash

1. Run `start ota` on the repeater again if it rebooted.
2. Run:

   ```
   dfu32> flash --yes
   ```

DFU32 waits up to 10 minutes for the target, enters the bootloader, checks the SoftDevice requirement, and streams the image. It prints:
- per-phase timings;
- throughput;
- `FLASH OK`, or the failure and whether the repeater's app was erased.

Afterwards, run `ver` on the repeater.

To cancel, press **Ctrl+C** (or Ctrl+D) in the console. `scan`, `probe` and `blreset` stop the same way, and so does an API job stopped with `POST /jobs/current/abort` or `job abort`.
- While DFU32 is still waiting for the target, or anywhere before START, the repeater's app is left untouched. If the repeater has already jumped into its bootloader, DFU32 sends SYS_RESET so it restarts the app (after a jump that can take up to 5 s more). If that fails, the abort message says so; run `blreset`.
- During the transfer, DFU32 stops before the next packet and sends SYS_RESET. The app is already erased by then; run `flash --yes` again (an OTAFIX bootloader waits in BLE OTA mode, usually at MAC+1).
- Once the last packet is sent, the flash finishes instead (a few seconds).
- A BLE connect already in progress finishes first (10 s at most).

An aborted flash is recorded as `FAILED` with failure kind `ABORTED`. `idf.py monitor` passes Ctrl+C to the board; exit the monitor with Ctrl+].

Tuning knobs:

| Option | Default | Use |
|---|---|---|
| `--prn N` | 8 | Packets per receipt. Try 4 if transfers stall. |
| `--packet N` | automatic | Cap the packet size, e.g. `--packet 20` to compare throughput. |
| `--interval MS` | 20 ms | Request a fixed BLE connection interval. Shorter means more packets per second, but **on OTAFIX, 11.25 ms and below overrun the bootloader** (see results). `probe` takes it too, and both report the interval actually negotiated. |
| `--settle MS` | 500 | Delay between START and the size packet. |
| `--attempts N` | 3 | Total attempts (1–10). After a stream overrun or link loss, the next attempt uses a 30 ms interval, then halves PRN. |
| `--force-prn` | off | Allow PRN above 8. It overruns OTAFIX at fast intervals. |
| `--wait S` | 600 | How long to wait for the target. |
| `--wifi on\|off` | on | WiFi stays up during the flash (SPEC §7.1). `--wifi off` stops it for the transfer and restores it afterwards. The run record's `wifi_off` shows which was used. |

## 6. Over the HTTP API (M3b)

Everything above can be done with `curl` from the PC. DFU32 answers on `http://dfu32-xxxx.local/` (the name is in the boot log and in `wifi`); use the IP if your OS doesn't resolve `.local`. Set a shell variable first:

```bash
DFU=http://dfu32-xxxx.local/api/v1
```

**Claim the device once.** The token is shown only this time; DFU32 keeps just its SHA-256. If you lose it, `token rotate` on the console prints a new one (`token clear` unclaims).

```bash
curl -s -X POST $DFU/claim
```

```bash
TOKEN=<token from the reply>
```

Every other call sends `-H "Authorization: Bearer $TOKEN"`. `GET /status` works without it.

**Set up the target** (one target in v1; a target made on the console takes the first id you PUT):

```bash
curl -s -X PUT $DFU/targets/t096 -H "Authorization: Bearer $TOKEN" -d '{"name":"Bench T096","board":"heltec_t096","mac":"F4:1C:36:A7:33:95","manifest_url":"http://<pc-ip>:8000/t096/manifest.json"}'
```

Tuning knobs go in the same record: `prn` (1–8), `max_attempts` (1–10), `arm_timeout_s` (10–3600), `start_settle_ms`, `prn_timeout_ms`, `max_app_size`.

**Check, stage, flash:**

```bash
curl -s -X POST $DFU/targets/t096/check -H "Authorization: Bearer $TOKEN"
```

```bash
curl -s -X POST $DFU/targets/t096/flash -H "Authorization: Bearer $TOKEN" -d '{"wifi_off":true}'
```

The flash call returns `202` straight away and the job runs on its own: it fetches (or reuses) the image, verifies it, and arms (`AWAITING_TARGET`). Now send `start ota` to the repeater, over USB or **remotely over LoRa from the companion app** (that's the M3b "done when"). Poll the job:

```bash
watch -n1 "curl -s $DFU/status"
```

With `"wifi_off":true` the API goes quiet from the moment the target is seen until WiFi is back (about a minute for the T096 image). With `"wifi_off":false` `status` shows the live phase, bytes and rate. Body fields, all optional: `source` (`manifest` or `staged`), `arm_timeout_s`, `wifi_off` (default: config `wifi_off_dfu`).

Other calls:

| Call | What it does |
|---|---|
| `POST /targets/t096/stage` | Stage without arming (`{"force":true}` re-downloads). Async; poll `status`. |
| `POST /jobs/current/abort` | Stop the job. During the transfer this leaves the target erased (the reply says so); flash again with `{"source":"staged"}`. |
| `GET /targets/t096/runs` · `GET /runs/7` | Run records; the second includes the log. |
| `GET /staging` (`?verify=1` re-hashes) · `DELETE /staging` | The staged image. |
| `POST /ble/scan` with `{"duration_s":10,"dfu_only":true}` | Advertisers, strongest first, with `dfu` and `matches_target` hints. Run `start ota` first. |
| `GET /logs` then `GET /logs?since=<next>` | Live log tail. |
| `GET /config` · `PUT /config` | `hostname`, `ntp_server` (both need a reboot), `wifi_off_dfu`, `dfu_deadline_max_s`. |
| `POST /system/reboot` · `POST /system/token/rotate` | |

The console's `job` shows the same state as `status`; `job abort` aborts.

### M3b bench checklist

`tools/dfu32_bench.py` runs this whole list for you; see [§7](#7-automated-bench-run).

- **Happy path with curl only:** claim, PUT target, flash, `start ota` from the companion app, `status` ends `SUCCEEDED`; `GET /targets/t096` shows `last_ok_version`.
- **Bad hash:** edit `bin.sha256` in the manifest, `POST /stage` with `{"force":true}`. `status` ends `FAILED` with `HASH_MISMATCH`.
- **Wrong board:** `PUT` the target with `"board":"rak4631"`, then flash. `FAILED`, `BOARD_MISMATCH` (from the manifest, or from the staged header with `"source":"staged"`), and the repeater is never touched.
- **Arm timeout:** flash with `{"arm_timeout_s":30}` and don't run `start ota`. `FAILED`, `TARGET_NOT_FOUND`, with a run record.
- **Busy:** while a job runs, a second `flash`, `stage`, `ble/scan` or target `PUT` gets `409`; so does the console's `scan`.
- **Abort while armed:** `POST /jobs/current/abort` during `AWAITING_TARGET`: `FAILED`, `ABORTED`, target untouched.
- **Offline re-flash from staging:** stage while the AP is up, start a flash with `{"source":"staged"}`, then switch the access point off and run `start ota`. Follow it on the console with `job`. It must succeed without the network. Because WiFi was already down when the target was seen, DFU32 doesn't insist on reconnecting afterwards (no reboot); `status` answers again once the AP is back.
- **Restore watchdog and run deadline** (needs the debug build: the `dfu32-<board>-debug` CI artifact, or `idf.py --preset <profile> -B build/<profile>-debug -DDFU32_DEBUG_ENDPOINTS=ON build`; never ship it):
  - `POST /debug/fault` `{"restore_fail":true}`, then flash with `"wifi_off":true`. After the transfer, DFU32 restarts WiFi and counts the restore as failed, retries once, then reboots. Each wait ends as soon as WiFi reconnects (60 s at most), so on a working network this takes seconds, not minutes. The run record is already written and WiFi comes back after the reboot.
  - `{"deadline_s":20}`: the run is aborted 20 s after the target is seen, recorded as `DEADLINE_EXCEEDED`, and WiFi is restored.
  - `{"stall_s":40}`: the DFU task blocks without a heartbeat; the stall watchdog reboots after about 30 s and the next boot records `ABORTED_BY_REBOOT`.
- **WiFi on vs off:** at least 5 runs each with `"wifi_off":true` and `false`. Compare `rate_x10_kBps`, attempts and failures in `GET /targets/t096/runs`. Done 2026-11-07 (results below): WiFi off is about 20% faster, but both finish in under a minute with no failures, so `wifi_off_dfu` defaults to off (WiFi stays on).

## 7. Automated bench run

`tools/dfu32_bench.py` runs the §6 checklist (all but the offline re-flash, which needs the access point switched off), plus the M2b run-record checks, from your computer. It types on DFU32's console over USB serial, calls the HTTP API, serves the firmware itself (with deliberately broken copies for the failure cases), and uses esptool when a check needs a different DFU32 build. It only stops for things it can't do:

- **`start ota` on the repeater.** It rings the terminal bell, prints what to do, and carries on by itself once DFU32 sees the repeater. With `--repeater-port` it sends `start ota` over the repeater's USB console instead. Check 3.5, the M3 "done when", always asks you to send it **over LoRa from the companion app**.
- Nothing needs the access point switched off. The WiFi-drop test (§4) and the offline re-flash (§6) are left to do by hand.
- **Fully unattended:** with `--repeater-port` and `--unattended`, nothing waits for you. 3.5 sends `start ota` over USB instead of LoRa (the report says so), and any check that would need a person is skipped.

You need Python 3.8+ and esptool, which brings pyserial with it: `pip install esptool`. Close any serial monitor first, so the port is free.

### Run it

Download the CI artifacts for your board. `dfu32-<board>` contains the merged image and the app-only `dfu32.bin`. `dfu32-<board>-debug` contains `dfu32-<board>-debug.bin`: the same app with the fault endpoints. It is a separate artifact so it can never be shipped by mistake.

```bash
python3 tools/dfu32_bench.py --port /dev/cu.usbserial-575E0522651 \
  --zip Heltec_T096_repeater-v1.18.0.zip --board heltec_t096 --mac F4:1C:36:A7:33:95 \
  --wifi-ssid MyNetwork --wifi-pass MyPassword \
  --merged-bin dfu32-feather_esp32_v2-merged.bin --app-bin dfu32.bin \
  --debug-app-bin dfu32-feather_esp32_v2-debug.bin
```

- `--merged-bin` is the one-off flash for the new layout. It wipes NVS, so the script joins WiFi again with `--wifi-ssid`/`--wifi-pass` (or set `DFU32_WIFI_PASS`). Leave it out on later runs, and the app-only `--app-bin` is flashed instead, which keeps WiFi, the token and the target. Leave both out to test whatever is already on the board.
- Section 4 flashes `--debug-app-bin` and puts `--app-bin` back afterwards. Without a debug build, section 4 is skipped.
- The script serves the firmware on port 8000 (`--site-port`) at your computer's LAN address. If DFU32 can't reach it, pass `--host-ip`.
- `python3 tools/dfu32_bench.py --list` lists the checks. `--only 3.1,3.5`, `--sections 1,2`, `--skip 3.11` and `--minimum` pick a subset. `--minimum` is what unblocks M4: 3.1, 3.2, 3.5 and 5.1.
- `--compare-runs N` sets how many flashes 5.1 does per WiFi setting (default 3, so 6 flashes; the WiFi default is already decided, so this now only watches for regressions).

A full run flashes the repeater about 20 times and takes roughly an hour, most of it in transfers and waiting for `start ota`. Ctrl+C stops after cleaning up: it aborts a running flash at a safe point, the same as typing Ctrl+C on the console.

### What it changes on DFU32

- It needs an API token, so it claims the device, or takes a new one with `token rotate`. **Any token you were using stops working.** The new one is saved as `token.txt` in the results folder. 3.1 also unclaims the device and claims it again.
- It creates or overwrites the target (`--target-id`, default `t096`). If another target id exists, it is deleted, because v1 allows only one.
- 3.13 renames DFU32 to `bench-dfu`, reboots, then renames it back and reboots again (`--keep-hostname` skips the rename back).
- The staged image ends up as the image under test.

### What you get

Each run writes a new folder under `bench-results/` (`--out` overrides it):

- `report.md`: one line per check (PASS, WARN, FAIL, SKIP or ERROR), then everything each check verified. 5.1 ends with the WiFi on/off comparison table.
- `report.json`: the same, with run records, timelines and samples.
- `serial.log`: everything DFU32 printed, timestamped, with the script's own notes (`###`) in between.
- `http.log`: every API call and its response.
- `wifi_compare.csv`: the 5.1 runs.
- `soak.csv`: the 6.1 runs, one row each, written as the soak goes.

WiFi passwords and API tokens are replaced with `<redacted>` in all of these, so the folder can be sent back as it is.

A WARN means the device did what it should but something looked off: a timing outside the expected range, an mDNS name that didn't resolve on your computer, or an 8 ms interval that happened not to overrun. The note says which.

### Order and dependencies

The checks run in checklist order, except that 3.3 (BLE scan) runs right after 3.5. The scan needs `start ota`, and a repeater left in OTA mode would let 3.5 start without the LoRa command it is meant to test. When a check needs the repeater *not* to advertise (3.7, 3.8, 3.10) and an earlier `start ota` hasn't been used up by a flash, the script asks you to reboot the repeater first, or sends `reboot` over `--repeater-port`.

Each check sets up what it needs (WiFi, token, target, staged image), so any of them can run on its own with `--only`. After a check that leaves the repeater's app erased (1.3, 3.9, 4.2), the script recovers it by flashing the staged image again. No `start ota` is needed for that, because the OTAFIX bootloader is waiting.

### Soak (M6)

Check 6.1 is the M6 soak: N flashes in a row, alternating between two versions, with faults mixed in. It only runs when you ask for it with `--soak N`; on its own, `--soak` runs just the soak.

```bash
python3 tools/dfu32_bench.py --port /dev/cu.usbserial-575E0522651 --repeater-port /dev/cu.usbmodem1101 --unattended \
  --zip Heltec_T096_repeater-v1.18.0-81dbae5.zip --soak-zip Heltec_T096_repeater-v1.17.0-0a1b2c3.zip \
  --board heltec_t096 --mac F4:1C:36:A7:33:95 --soak 20 \
  --debug-app-bin dfu32-feather_esp32_v2-debug.bin --app-bin dfu32.bin
```

- Odd runs flash `--zip`, even runs `--soak-zip` (`--soak-bin`/`--soak-dat` work too), so every successful flash changes the version. Each run points the target at that image's manifest and flashes with `source: manifest`, so the download and staging are part of every run. Without a second image every run flashes the same one, and the report warns.
- Every third run (`--soak-fault-every`) gets the next fault in turn from `--soak-faults` (default `reset,abort,deadline,stall,restore`; `none` for clean runs only):
  - `reset`: DFU32 is reset at 20% of the transfer (expects `ABORTED_BY_REBOOT`);
  - `abort`: `POST /jobs/current/abort` at 20% (expects `FAILED ABORTED`);
  - `deadline`, `stall`, `restore`: the §14.2 debug faults, as in checks 4.1-4.3. They need the debug build (`--debug-app-bin`, put back with `--app-bin` afterwards) and are left out, with a note, without it.
- After a run that didn't flash, the soak flashes the staged image again (`source: staged`), as you would by hand. A run counts as **recovered** when the target's last run is a new `SUCCEEDED` record and `last_ok_version` is that run's image. With `--repeater-port`, the repeater's `ver` must also show it when the version contains `v<x.y.z>-<commit>` (as `dfu32_meshcore_fetch.py` and MeshCore's zip names do).
- 6.1 fails if any run is not recovered; that is the M6 bar (zero unrecovered failures on OTAFIX). A run without a fault that needed a recovery flash is a WARN. If the repeater stops answering `ver` after a failed run, the soak stops there.
- Without `--repeater-port` you'll be asked for `start ota` on every run, so use it with `--unattended` for a soak. 20 runs on the T096 take about 30-40 minutes.

### Developing the runner

`tools/bench/fake_dfu32.py` is a rough simulator of DFU32's console and API. `tools/test_bench.py` runs the script against it, together with unit tests for the output parsers. These run in CI with the other tool tests, on a subset of the checks. `DFU32_BENCH_SIM=all` runs every check (about 2.5 minutes). When the real device and the simulator disagree, fix the simulator.

## 8. Provisioning, LEDs and button (M4a)

These need a phone or a computer with Bluetooth, and eyes on the board, so the bench runner doesn't cover them yet.

**Improv BLE.** Use the Home Assistant companion app (Settings → Devices → "Discovered"), or https://www.improv-wifi.com/ in desktop Chrome or Edge ("Connect device to Wi-Fi").

1. **Fresh device:** flash the merged bin (it wipes NVS), or run `wifi forget` and reboot. The boot log says "no WiFi network stored", `improv` shows "open", and the NeoPixel blinks white slowly.
2. **Provision:** pick "DFU32" (or `dfu32-xxxx`) in the Improv client and enter the SSID and passphrase. Within about 10 s the client shows success and offers `http://<ip>/`, the NeoPixel goes dim green, and `wifi` shows the network. About 5 s later `improv` shows "closed".
3. **Wrong passphrase:** after `wifi forget` and `improv start`, send a wrong passphrase. After about 30 s the client shows "unable to connect"; retrying with the right one works. If a network was stored before, `wifi` shows it again after the failed attempt.
4. **Identify:** the Improv web page's identify, if offered, blinks the NeoPixel white quickly for 10 s.
5. **Busy:** while Improv is open, the console's `scan` says "Improv BLE provisioning is open", and `POST /flash` answers `409` with holder `improv`. `improv stop` frees it.
6. **Device info:** the Improv web page shows DFU32, the firmware version (from `git describe`, e.g. `v0.3.0-12-gabc1234`), the chip and the hostname. `/api/v1/status` shows the same version.

**Button** (BOOT on the Feather S3 and N16R8, GPIO38 on the Feather V2). Watch the NeoPixel while holding:

7. **Hold 2–5 s:** amber slow blink while held; on release `improv` shows "open" and the NeoPixel blinks white. It closes by itself after 5 minutes.
8. **Hold 5–15 s:** amber fast blink; on release the log says "API token cleared", `GET /status` shows `"claimed":false`, and `POST /claim` works again.
9. **Hold 15 s or more:** solid amber; on release DFU32 erases its settings and WiFi and reboots into Improv. `runs` and `staged` still show the run history and the staged image.
10. **Short press (< 2 s):** nothing happens.

**LEDs** during an API flash with `"wifi_off":false`: blue pulse while fetching or verifying, cyan blink while armed, magenta getting brighter during the transfer (the red LED blinks), solid green for 10 s on success. For a failure, abort mid-transfer: the NeoPixel blinks red fast and the red LED stays on until a later flash succeeds. A failure before START (e.g. arm timeout) shows solid red until the next job. With the access point off, the NeoPixel blinks dim yellow.

## 9. Web UI and firmware update (M4b)

Open `http://dfu32-xxxx.local/` (or the IP) in a desktop and a phone browser.

1. **Claim:** on an unclaimed device the page offers **Claim this device** and shows the token once. Reload: you are still signed in. In a private window you get **Sign in**; a wrong token says so, the right one works.
2. **Improv redirect:** after provisioning over Improv BLE (§8), the client's "visit device" link opens this page on the claim screen.
3. **Target:** run `start ota`, **Scan**, **Use as target** fills the MAC; fill in id, board and manifest URL, **Save**. Reloading shows the saved values. Change one tuning value and save.
4. **Happy path from the UI only:** **Check** shows the version and board match; **Flash**, then `start ota` over LoRa from the companion app. The job card shows armed (countdown), then the progress bar and rate, then succeeded, with a link to the run. This is the M4 "done when" (after joining WiFi over Improv).
5. **Abort mid-transfer:** the page asks first and then warns that the repeater is erased; **Flash** with *staged image* recovers it.
6. **Runs:** the list shows the flashes; a run's page has the attempts and the log. **Logs** tails live while a flash runs.
7. **Firmware update:** **System → Firmware update** with the CI `dfu32.bin` for this board. The bar fills, the device reboots, and the page says "Back up on <version>". `runs`, the target and WiFi are kept. Start a flash within a second of the reboot announcement: it is refused as busy.
8. **Wrong image:** upload another board's `dfu32.bin` (e.g. the N16R8 build to a Feather S3): refused before anything is written, naming both boards, and the page shows that error (not a connection reset). The merged `.bin`: refused as "not an ESP-IDF app image".
9. **Rollback rules:**
   - With the access point on throughout, the console says "new firmware confirmed: WiFi and the HTTP API are up", and a later reboot stays on the new version.
   - Switch the access point off as soon as the upload finishes. After 90 s the console says "confirmed: WiFi works but … isn't reachable" (a router outage is not the firmware's fault), and it stays on the new version once the access point is back.
   - A real rollback needs a build that breaks itself, which a normal build can't do. Use the console log: a self-check failure, or "shows no WiFi activity", followed by "rolling back to the previous image", and the page reports the old version.
10. **Busy:** while a flash runs, **Install** answers busy.

## Bench results so far

| Date | DFU32 | Target / bootloader | Image | Settings | Stream | Total |
|---|---|---|---|---|---|---|
| 2026-11-04 | Feather V2 (1M PHY) | Heltec T096, OTAFIX 2.3 (MAC+1 after jump) | 413,680 B MeshCore repeater | 244 B, PRN 8, default interval | 44.9 s (9.2 kB/s) | 53.0 s, `ver` OK |
| 2026-11-04 | Feather V2 (1M PHY) | Heltec T096, OTAFIX 2.3 | same | 244 B, PRN 16, default interval | 41.8 s (9.8 kB/s) | 50.4 s |
| 2026-11-04 | Feather V2 (1M PHY) | Heltec T096, OTAFIX 2.3 | same | 244 B, PRN 30, default interval | 40.4 s (10.2 kB/s), 1,206 TX back-off retries | 49.4 s |
| 2026-11-05 | Feather V2 (1M PHY) | Heltec T096, OTAFIX 2.3 | same | 244 B, PRN 16, **interval 7.5 ms** | link lost after 2.3 s (bootloader reset) | **failed** |
| 2026-11-05 | Feather V2 (1M PHY) | Heltec T096, OTAFIX 2.3 (already in BL at MAC+1) | same | 244 B, PRN 16, **interval 7.5 ms** | 0.4 s at 29.5 kB/s, then `10 03 06` OPERATION_FAILED | **failed** |
| 2026-11-05 | Feather V2 (1M PHY) | Heltec T096, OTAFIX 2.3 | same | 244 B, PRN 8, **interval 7.5 ms** | link lost about 4.5 s in | **failed** |
| 2026-11-05 | Feather V2 (1M PHY) | Heltec T096, OTAFIX 2.3 | same | 244 B, PRN 8, **interval 11.25 ms** | link lost about 3.4 s in | **failed** |
| 2026-11-05 | Feather V2 (1M PHY) | Heltec T096, OTAFIX 2.3 | same | 244 B, PRN 8, default range → **30 ms negotiated** | 44.8 s (9.2 kB/s) | 51.0 s |
| 2026-11-05 | Feather V2 (1M PHY) | Heltec T096, OTAFIX 2.3 | same | 244 B, PRN 8, **interval 20 ms** | **33.2 s (12.4 kB/s)** | **38.4 s** |
| 2026-11-05 | Feather V2 (1M PHY) | Heltec T096, OTAFIX 2.3 | same | 244 B, PRN 8, **interval 15 ms** (2 runs, both OK) | 27.9 s (14.8 kB/s) | |
| 2026-11-05 | Feather V2 (1M PHY), **M2a retries** | Heltec T096, OTAFIX 2.3 | same | `--interval 8` | attempt 1: LINK_OR_OVERRUN after 27 KB; attempt 2 at 30 ms: 44.9 s | **61.0 s, recovered by itself** |
| 2026-11-05 | Feather V2 (1M PHY), **M2a retries** | Heltec T096, OTAFIX 2.3, **power-cycled at ~12%** | same | default (20 ms) | attempt 1: link lost after 52 KB; attempt 2 found the bootloader at MAC+1, 30 ms: 45.2 s | **65.3 s, recovered by itself** |
| 2026-11-07 | Feather V2 (1M PHY), API, **WiFi off** (5 runs, bench runner 5.1) | Heltec T096, OTAFIX 2.3 | 413,680 B MeshCore repeater v1.18.1 | default (20 ms), staged | 12.3 kB/s mean, all first attempt | **39.0 s mean, 5/5** |
| 2026-11-07 | Feather V2 (1M PHY), API, **WiFi on** (5 runs, bench runner 5.1) | Heltec T096, OTAFIX 2.3 | same | default (20 ms), staged | 10.2 kB/s mean, all first attempt | **47.2 s mean, 5/5**: chosen default (live progress, under a minute) |

PRN 8 → 30 gains only about 11%. Throughput sits around 41 packets/s regardless, which looks like about one 244-byte packet per connection event. At PRN 30, NimBLE's TX buffers also fill up; the back-off retries absorb it. So the radio schedule is the limit, not receipt round trips or nRF flash.

**At a 7.5 ms interval the radio outran the bootloader.**
- OTAFIX sends a receipt when a packet *arrives*, not when it's written to flash. Its lazy-erase path holds only **8 pending packets**, and a page erase (about 85 ms) stalls writes.
- With PRN 16, up to 16 unwritten packets can pile up, giving `10 03 06` OPERATION_FAILED or a bootloader reset.
- PRN 8 did **not** fix it at 7.5 or 11.25 ms. Receipts acknowledge *arrival*, so DFU32 keeps sending while earlier packets still wait behind a lazy page erase (about 85 ms per 4 KB page). The PRN window therefore does not bound OTAFIX's pending writes.
- What bounds them is the arrival rate during an erase: at most about 8 packets per 85 ms, roughly 94 packets/s or about 23 kB/s (an estimate from the source, not measured). The default interval (about 41 packets/s observed) stays well inside that.
- **Measured:** throughput scales with 1/interval at about **one 244-byte packet per connection event** (30 ms: 38 packets/s, 20 ms: 51 packets/s). That suggests data length extension isn't in effect, to be checked in M2.
  - The default 15–30 ms range negotiates 30 ms.
  - 11.25 ms works out to about 89 packets/s, right at the ~94 packets/s OTAFIX erase limit, which fits it failing.
  - 20 ms works (+35% over the default). 15 ms (about 67 packets/s) is the next test.
- Options:
  - an interval between the old default and 11.25 ms. 20 ms and 15 ms both work. **20 ms is now the default**, roughly 2× under the erase limit; 15 ms is faster but has less margin;
  - pacing around 4 KB page boundaries, an experimental `--page-pause` idea;
  - M2 retries falling back to the default interval and PRN after a link loss or `06` in the stream.
- The 2 s supervision timeout cut the buttonless jump from 4.5 s to 2.4 s.

Next to test:
- `--interval` (the build that also logs the negotiated interval);
- whether data length extension is actually active;
- 2M PHY (Feather S3).

## Run history

Every `flash` is recorded in the `runlog` partition (14 slots on 4 MB flash, 16 on 8 MB; the oldest is overwritten):

- `runs`: one line per run (result, duration, attempts, whether the target was erased, version or error).
- `run N`: that run's full JSON summary and its log.
- `logs [bytes]`: recent captured log output (default 4 KB).

If DFU32 reboots in the middle of a flash, the next boot logs a warning and records `ABORTED_BY_REBOOT`. When the transfer had begun, the record has `target_erased: possibly`; run `flash --yes` again.

## If `probe` can't find the bootloader after the jump

DFU32 logs every DFU-looking advertiser it saw, by name, legacy DFU UUID or Secure DFU UUID. Then:
- If nothing was seen, run `scan 10` right away and look for a `*DFU*` name or anything near the repeater's MAC. On the repeater, `get bootloader.ver` (before probing) tells us which bootloader it runs.
- If the board is stuck in its bootloader (on OTAFIX and older upstream it advertises forever), run `blreset` or power-cycle it. Nothing has been erased.
- `Secure DFU FE59` means the board's bootloader uses Nordic Secure DFU, which DFU32 does not support.

## If a flash fails

- After a failure DFU32 sends SYS_RESET. An OTAFIX bootloader then comes back in BLE OTA mode, usually at **MAC+1**, and DFU32 finds it there. Run `flash --yes` again; no `start ota` is needed, because the app is gone and the bootloader is waiting.
- If START reports `INVALID_STATE`, run `blreset`, then `flash --yes`.
- If the board is in USB DFU (upstream bootloader ≤ 0.9.x, or after a power cut), double-tap reset and copy the MeshCore `.uf2` to the drive that appears.
