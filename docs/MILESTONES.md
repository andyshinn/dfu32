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

## M1: Smallest end-to-end proof: flash one known-good image (2–3 days)

The goal is to prove the BLE protocol path and nothing else. No WiFi, no HTTP, no UI.
- `tools/dfu32-stage-image`: takes a MeshCore DFU `.zip` and writes a staging partition image (§8 header + bin). Flash it with `parttool.py write_partition --partition-name staging`.
- Firmware: NimBLE central, plus the target MAC and timeouts from Kconfig/menuconfig. On a BOOT button press:
  1. load the staged image and verify it (SHA-256, `.dat`, CRC16);
  2. scan for app MAC / MAC+1 with the DFU UUID;
  3. do the buttonless jump (`01 04`) and re-scan;
  4. read the Version and DIS characteristics and log them;
  5. run START → INIT → PRN → RECEIVE → stream → VALIDATE → ACTIVATE with fixed PRN 8.
- Everything is logged to the console, including MTU, chunk size, DIS string, timings per phase, and throughput.
- **Done when:** on the XIAO you run `start ota`, press BOOT on the Feather, the repeater reboots, and `ver` shows the flashed version. Done three times in a row.
- **Also answers:** open questions 1, 7 and 8, and an initial throughput number. Based on these, update SPEC §5/§6 defaults.

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

## M3: Networking & API (3–4 days)

- `net_mgr`: WiFi STA, Improv Serial, mDNS, SNTP, stop/restore, and the run deadline + restore verification (§7.3).
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

## M4: UI, provisioning & polish (3–4 days)

- Web UI: status, target form with the **scan picker**, runs, logs, system.
- Improv BLE (only while unprovisioned), the button (unclaim / factory reset), and the LED states.
- DFU32 self-OTA upload with rollback (`esp_ota_mark_app_valid_cancel_rollback` once WiFi and HTTP are up).
- ESP Web Tools manifest so DFU32 can be flashed from a browser, plus Improv Serial provisioning in the same session.
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
