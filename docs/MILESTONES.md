# Milestone plan

Each milestone ends with something demonstrable on the bench.

**Bench kit:**
- Feather ESP32-S3 #5477 (4 MB flash / 2 MB PSRAM) running DFU32
- XIAO nRF52840 with the OTAFIX bootloader and a MeshCore repeater build (sacrificial). Match the production repeaters: OTAFIX 2.2 (`0.9.2-OTAFIX2.2-BP1.3`) or newer.
- A PC on the XIAO's USB CDC port for `start ota` and `ver`, and for UF2 recovery

## M0: Skeleton (½ day)

- IDF v6.1 project, `sdkconfig.defaults` plus the `feather_esp32s3` / `feather_esp32_v2` / `esp32s3_n16r8` profiles, `partitions/4mb.csv` and `8mb.csv`.
- `idf_component.yml` with `esp-nimble-cpp` pinned to 2.6.0.
- GitHub Actions: build esp32s3 + esp32, enforce the app size budget (fail above 1.30 MB on the 4 MB profile), run host unit tests.
- `components/dfu_proto` with CRC16, the `.dat` parser, the chunk planner and host tests (§14.1 subset).
- **Done when:** CI is green and the host tests pass.
- **Status:** implemented.
  - Host tests pass on GCC 13 and Clang. They include CRC16 checks against four real MeshCore v1.18.0 images.
  - The partition and size-budget scripts are tested.
  - The firmware build is verified by CI and on the developer's machine, not in the authoring environment (no ESP-IDF toolchain there).
  - Beyond the plan, `dfu_proto` also gained the Control Point encoders and notification decoder that M1 needs.

## M1: Smallest end-to-end proof: flash one known-good image (2–3 days)

The goal is to prove the BLE protocol path and nothing else. No WiFi, no HTTP, no UI.
- `tools/dfu32_stage_image.py`: takes a MeshCore DFU `.zip` (or `.bin` + `.dat`), validates it, and writes a staging-partition image (§8 header + bin). Flash it with esptool or `parttool.py`.
- Firmware: NimBLE central plus a **USB-serial bench console**. This replaces the planned BOOT-button trigger, because it gives a safer and more useful test tool. Commands:
  - `target <MAC>`: store the repeater's app MAC.
  - `scan`: list advertisers.
  - `staged`: show and verify the staged image (SHA-256, `.dat`, CRC16, prechecks).
  - `probe`: the non-destructive link test. It finds the target (MAC or MAC+1), does the buttonless jump, and reads Version, DIS, MTU and PHY. It reports the packet size, then sends SYS_RESET. It never sends START.
  - `flash --yes`: one full attempt. The SD requirement is checked before START. The transfer runs START → INIT → PRN → RECEIVE → stream → VALIDATE → ACTIVATE (PRN 8 by default; `--prn`, `--packet` and `--settle` are tunable). It sends SYS_RESET after a failure.
  - `blreset`: send SYS_RESET to a bootloader left mid-session.
- Transfer logic lives in `dfu_proto::run_transfer()` behind an `ITransport` interface. It is host-tested against a simulated bootloader (`test/host/fake_bootloader.hpp`).
- Everything is logged, including MTU, packet size, DIS string, per-phase timings and throughput.
- **Done when:** on the XIAO you run `start ota`, run `flash --yes` on the Feather, the repeater reboots, and `ver` shows the flashed version. Done three times in a row.
- **Also answers:** open questions 1, 7 and 8 (run `probe` first), and an initial throughput number. Based on these, update SPEC §5/§6 defaults.
- **Status:** **first end-to-end flash succeeded on 2026-11-04.** A Feather V2 flashed a Heltec T096 on OTAFIX 2.3 with a 413,680 B MeshCore repeater image in 53 s (stream 9.2 kB/s), and `ver` confirmed it. Runs 2 and 3 (PRN 16 and 30) also succeeded, so the **"three in a row" criterion is met**. Throughput was 9.2 / 9.8 / 10.2 kB/s. Connection-interval tuning continues into M2. See [BENCH.md](BENCH.md).
  - Host tests pass (48 C++ tests, 10 Python), including the transfer engine against the simulated bootloader.
  - The firmware build is verified by CI.

## M2: Robust DFU client (3–4 days)

- The full client FSM (§6.4): every response check, a per-op timeout, PRN byte-count verification, ENOMEM back-pressure handling, and the `.len ≤ mtu−3` assertion.
- Recovery (§6.5): SYS_RESET, MAC+1 re-scan, backoff, configurable attempts. Bootloader classification and the `SD_REQ_MISMATCH` precheck before START.
- All §7.2 prechecks. The `job_active` NVS flag and `ABORTED_BY_REBOOT` detection.
- The `runlog` partition and run records; the log ring.
- Mock-transport state machine tests on the host (§14.1).
- *Optional, recommended:* the `test/fakeboot` fixture on the Feather V2 (§14.3).
- **Bench:**
  - link drop mid-transfer leads to automatic recovery and success;
  - power cut mid-transfer, then reflash from staging;
  - bad `.dat` / SD mismatch is refused before START;
  - the throughput matrix (PRN × MTU × PHY). WiFi on vs off is compared in M3, once WiFi exists.
- **Done when:** the §14.4 failure rows that don't need networking all pass, and the run records show the right error codes.
- **Split into two PRs:**
  - **M2a, robust flashing:**
    - retries with SYS_RESET between attempts;
    - backoff that degrades after overruns (from the M1 bench findings);
    - PRN cap of 8, stale-session recovery, target-gone detection;
    - `flash --attempts N`;
    - the retry policy is pure, host-tested logic (`dfu_proto/retry.hpp`).
  - **M2b, observability:**
    - run records in `runlog`, the log ring, `job_active` reboot detection;
    - `runs`, `run N` and `logs` console commands;
    - the transfer wakes immediately on disconnect, instead of waiting out the receipt timeout (about 3 s saved per failed attempt).

    The pure parts (`LogRing`, slot codec, ring rotation, JSON) are host-tested in `components/runlog`.

## M3: Networking & API (3–4 days)

- `net_mgr`: WiFi STA, mDNS, SNTP, stop/restore, and the run deadline + restore verification (§7.3). Improv Serial moved to M5, with the browser installer: it shares the console port with the REPL and is only useful from the installer page.
- `fetcher` + `manifest`: HTTP fetch into staging with streaming SHA-256 and read-back verification.
- Orchestrator job FSM (§7): check / stage / flash (arm, then wait) / abort.
- `api`: claim, token auth, status, targets, runs, logs, staging, **BLE scan** endpoint, system reboot/rotate.
- `tools/dfu32-pack` (zip → bin/dat/manifest.json).
- **Bench:**
  - happy path driven by `curl` from the PC;
  - bad hash;
  - wrong board;
  - arm timeout;
  - WiFi-restore watchdog (debug fault endpoint);
  - an offline re-flash from staging with the AP off;
  - **WiFi on vs off during the transfer** (`wifi_off` per request): at least 5 runs each, comparing rate, PRN timeouts and failures in the run records. This sets the shipped `wifi_off_dfu` default. **Done 2026-11-07:** WiFi stays on by default (47 s vs 39 s, no failures either way).
- **Done when:** a full flash is done end to end using only HTTP calls and `start ota` sent **remotely over LoRa** from a companion app.
- **Split into two PRs:**
  - **M3a, fetch and stage (implemented):**
    - `components/net`: WiFi STA (credentials in the IDF WiFi NVS), reconnect with backoff, mDNS `dfu32-xxxx.local`, SNTP, and `stop()`/`restore()` for the handover;
    - `components/fetch`: HTTP GET with redirects, manifest fetch, the `.dat` before anything is erased, then the `.bin` streamed into staging;
    - `components/dfu32_core` (pure, host-tested): a thin wrapper over `espressif/cjson`, URL resolution, manifest parse and validation, §7.2 prechecks, the staging writer (lazy sector erase, read-back, header last) and target-config validation;
    - `components/config`: target settings as one atomic NVS record (`tgt0`), reading an M1 target until the first save;
    - console: `wifi`, `target board|url|id|maxapp|clear`, `check`, `stage`;
    - `tools/dfu32_pack.py`, with a golden manifest shared with the C++ tests.
  - **M3b, orchestrator and API (implemented, awaiting bench):**
    - `components/job`: one job at a time on its own task (stage, or flash = arm then wait); a device-wide lease so the console and the API's scan can't collide with a job (`409 Busy`);
    - radio handover: WiFi stays up while fetching and waiting, and stops only once the target is seen (per request `wifi_off`, default config `wifi_off_dfu`);
    - SPEC §7.3 layers: the run deadline (`esp_timer`, `DEADLINE_EXCEEDED`), a stall watchdog fed by heartbeats from every blocking BLE wait (reboots after 30 s of silence), restore verification (retry once, then reboot), the `job_active` marker;
    - `components/api`: the §10.1 endpoints on `esp_http_server`, claim-on-first-use token (only its SHA-256 in NVS), constant-time compare, the 5-failure throttle, mDNS `_http._tcp`; fault injection behind `CONFIG_DFU32_DEBUG_ENDPOINTS`;
    - pure, host-tested parts in `dfu32_core`: the job state machine and request parsing, route matching, base64url, the auth throttle, device settings, per-target tuning knobs, scan hints;
    - run records gain `target_id`, `trigger`, `source` and `started_unix`;
    - console: `job [abort]`, `token [rotate|clear]`.

    The bench checklist is in [BENCH.md §6](BENCH.md#6-over-the-http-api-m3b).

## M4: UI, provisioning & polish (4–5 days)

- Web UI: status, target form with the **scan picker**, runs, logs, system. `GET /` works while unclaimed and opens on the claim screen, because that's where an Improv redirect lands.
- Improv core plus Improv BLE (only while unprovisioned), built on `improv-wifi/sdk-cpp` (SPEC §4.1, §9.2). Keep the core separate from the transport so M5 can add Improv Serial without changing it:
  - takes the WiFi settings, saves them with `net::set_credentials`, waits for the connection (with a timeout), then reports provisioned with the redirect URL `http://<ip>/`, or reports an error;
  - answers device info with `DFU32`, the firmware version, the chip and the hostname.
- The button (unclaim / factory reset) and the LED states, built on `espressif/button` and `espressif/led_indicator` + `led_strip` (SPEC §4.1) rather than local code.
- DFU32 self-OTA upload with rollback (`esp_ota_mark_app_valid_cancel_rollback` once WiFi and HTTP are up).
- Firmware version from the git tag: CI checks out full history (`fetch-depth: 0`) so `git describe` gives `v0.4.0` rather than a bare hash. The same string shows in `/api/v1/status`, the UI, Improv device info and, in M5, the installer manifests.
- `docs/RECOVERY.md` and `docs/OPERATOR.md` (MeshCore walkthrough, including remote `start ota`).
- **Done when:** a Feather flashed with `idf.py` joins WiFi over Improv BLE (for example from Home Assistant or Chrome) and then completes a repeater flash using only the web UI.

## M5: Browser installer (2–3 days)

A static page on GitHub Pages that flashes DFU32 over Web Serial and sets WiFi with Improv Serial in the same session. It needs desktop Chrome or Edge and a USB cable, and no toolchain. Details in SPEC §9.4.

- **Improv Serial in firmware**, on the console port (USB-Serial-JTAG on the S3, UART0 at 115200 on the classic ESP32), using the M4 Improv core:
  - the REPL currently uses `esp_console_new_repl_stdio`, so linenoise owns stdin. Replace it with a small reader: frames that start with `IMPROV` go to the Improv parser, and everything else goes to the line editor and `esp_console_run`. Typing in a terminal must work as it does today;
  - resync as in SPEC §9.2: a partial header goes back to the line editor; after the header, a bad checksum, an oversized length or 250 ms of silence drops the frame. The framing state machine is pure and host-tested;
  - it starts early enough in boot to answer within ESP Web Tools' post-install wait (10 s by default);
  - it stays on after provisioning, so the page can recognise an installed DFU32 and offer to change WiFi or open the device's page. USB access already allows this through the `wifi` console command.
- **`web/`**: `index.html` plus a little CSS. No framework and no build step.
  - The `<esp-web-install-button>` comes from ESP Web Tools, pinned to an exact version.
  - A board picker sets the button's `manifest`. One manifest can't serve every board, because the Feather S3 and the N16R8 are both `ESP32-S3`.
  - The button's `unsupported` slot shows a message in browsers without Web Serial.
  - A warning next to the button: choosing "Erase device" removes WiFi, the token and the target.
  - Links to the README, `OPERATOR.md` and `RECOVERY.md`.
- **`tools/dfu32_web_manifest.py`** (with unit tests, like the other tools) writes `manifest-<board>.json` from a build's `flasher_args.json`:
  - one build per board: its `chipFamily`, plus the separate parts at their offsets (bootloader, partition table, otadata, app). Not the merged `.bin`: raw merging fills the gaps with `0xFF`, so updating from the page would wipe NVS (WiFi, token, target) and `runlog`;
  - `new_install_prompt_erase: true`, so a fresh install can start from blank flash;
  - `version` is the M4 firmware version string.
- **Partition baseline:** `tools/check_partitions.py` checks `nvs`, `runlog` and `staging` against a committed baseline, so a layout change that would break keep-settings updates can't land by accident (SPEC §9.4).
- **CI**: a `pages` job in `ci.yml` (details in SPEC §9.4):
  - triggers on `release: published` and `workflow_dispatch`. A release run builds the firmware from the tag in the same run and uses that run's artifacts;
  - deploys only for the latest stable release, never a pre-release or a patch to an older line, under a `pages` concurrency group;
  - manual runs deploy from `main`. This is the path to bench M5 before the first release;
  - generates the manifests and copies `web/`, the parts and the manifests into one directory;
  - it publishes that directory with `actions/upload-pages-artifact` and `actions/deploy-pages`. The `.bin` files have to be served from Pages itself, because release-asset downloads don't send CORS headers;
  - it also attaches the merged `.bin` files to the release for esptool users;
  - one-time repo settings: Pages → Source → "GitHub Actions", and a `v*` tag rule in the `github-pages` environment's deployment branches and tags (by default it rejects tag refs).
- `THIRD_PARTY_NOTICES`, generated from `dependencies.lock`, ships on the page and with each release (moved here from M6, because this is the first milestone that publishes binaries).
- README: an "Install from your browser" link, and a note that the merged `.bin` is for fresh installs and resets settings.
- **Bench:**
  - from a manual `pages` deployment of `main`: Feather S3 (erased) and Feather V2: install, enter the SSID and passphrase, then "Visit device" opens the claim screen;
  - a wrong passphrase shows the Improv error, and the retry works;
  - coming back with DFU32 installed: the page recognises it, and updating keeps WiFi, the token, the target and the staged image;
  - the console REPL still works in a terminal afterwards, including after a cut-off Improv frame;
  - after the first release is published, the page serves it. Publishing a pre-release leaves the page alone.
- **Done when:** someone else can go from a fresh Feather to a successful repeater flash using only the browser (moved here from M4).

## M6: v1.0 hardening (2 days)

- Soak test: 20 consecutive flashes with alternating versions, mixed with injected faults. Zero unrecovered failures on OTAFIX.
- Validate on the Feather V2 (classic ESP32, BLE 4.2) and an N16R8 DevKit.
- `tools/dfu32-meshcore-fetch` (release asset resolution through the GitHub API).
- Fix defaults from the measurements (PRN, `wifi_off_dfu`, timeouts). Release builds with debug endpoints compiled out. Tag v1.0, which also publishes the installer page.

## Later (post-v1, in rough priority order)

1. MQTT + Home Assistant discovery (`update` entity, progress, Flash button), with `espressif/mqtt`.
2. Ed25519-signed manifests (backward-compatible `sig` field), verified with `espressif/libsodium`.
3. More than one target per device (raise `DFU32_MAX_TARGETS`, add a per-target staging slot or a shared slot with re-fetch).
4. On-device raw DFU `.zip` ingestion with miniz on 8 MB+ profiles.
5. Optional wired trigger plugin (UART / USB host) for unattended updates.
6. Optional HTTPS fetch (`DFU32_HTTPS`) with the IDF cert bundle.
