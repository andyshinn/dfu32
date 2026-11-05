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

- `net_mgr`: WiFi STA, mDNS, SNTP, stop/restore, and the run deadline + restore verification (§7.3). Improv Serial moved to M4, next to ESP Web Tools: it shares the console port with the REPL and is only useful from the browser installer.
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
  - **WiFi on vs off during the transfer** (`wifi_off` per request): at least 5 runs each, comparing rate, PRN timeouts and failures in the run records. This sets the shipped `wifi_off_dfu` default.
- **Done when:** a full flash is done end to end using only HTTP calls and `start ota` sent **remotely over LoRa** from a companion app.
- **Split into two PRs:**
  - **M3a, fetch and stage (implemented):**
    - `components/net`: WiFi STA (credentials in the IDF WiFi NVS), reconnect with backoff, mDNS `dfu32-xxxx.local`, SNTP, and `stop()`/`restore()` for the handover;
    - `components/fetch`: HTTP GET with redirects, manifest fetch, the `.dat` before anything is erased, then the `.bin` streamed into staging;
    - `components/dfu32_core` (pure, host-tested): a thin wrapper over `espressif/cjson`, URL resolution, manifest parse and validation, §7.2 prechecks, the staging writer (lazy sector erase, read-back, header last) and target-config validation;
    - `components/config`: target settings as one atomic NVS record (`tgt0`), reading an M1 target until the first save;
    - console: `wifi`, `target board|url|id|maxapp|clear`, `check`, `stage`;
    - `tools/dfu32_pack.py`, with a golden manifest shared with the C++ tests.
  - **M3b, orchestrator and API:** the job FSM (arm, then wait), WiFi handover with the run deadline and restore check, the HTTP API with token claim, the BLE scan endpoint, and the WiFi on/off bench comparison.

## M4: UI, provisioning & polish (3–4 days)

- Web UI: status, target form with the **scan picker**, runs, logs, system.
- Improv BLE (only while unprovisioned), the button (unclaim / factory reset), and the LED states.
- DFU32 self-OTA upload with rollback (`esp_ota_mark_app_valid_cancel_rollback` once WiFi and HTTP are up).
- ESP Web Tools manifest so DFU32 can be flashed from a browser, plus Improv Serial provisioning in the same session (moved here from M3; until then, set WiFi with the `wifi` console command).
- `docs/RECOVERY.md` and `docs/OPERATOR.md` (MeshCore walkthrough, including remote `start ota`).
- **Done when:** someone else can go from a fresh Feather to a successful repeater flash using only the browser.

## M5: v1.0 hardening (2 days)

- Soak test: 20 consecutive flashes with alternating versions, mixed with injected faults. Zero unrecovered failures on OTAFIX.
- Validate on the Feather V2 (classic ESP32, BLE 4.2) and an N16R8 DevKit.
- `tools/dfu32-meshcore-fetch` (release asset resolution through the GitHub API).
- Fix defaults from the measurements (PRN, `wifi_off_dfu`, timeouts). Release builds with debug endpoints compiled out. Tag v1.0.

## Later (post-v1, in rough priority order)

1. MQTT + Home Assistant discovery (`update` entity, progress, Flash button).
2. Ed25519-signed manifests (backward-compatible `sig` field).
3. More than one target per device (raise `DFU32_MAX_TARGETS`, add a per-target staging slot or a shared slot with re-fetch).
4. On-device raw DFU `.zip` ingestion with miniz on 8 MB+ profiles.
5. Optional wired trigger plugin (UART / USB host) for unattended updates.
6. Optional HTTPS fetch (`DFU32_HTTPS`) with the IDF cert bundle.
