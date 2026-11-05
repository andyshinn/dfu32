# Open questions & assumptions

Items are ranked by how much the design changes if the answer differs from the assumption.
- **Q** = needs your answer.
- **B** = needs a bench measurement.
- **A** = an assumption the spec runs on; confirm or correct it.

## Decisions already made (from the grilling session, for the record)

| Topic | Decision |
|---|---|
| OTA trigger | Manual (`start ota` over serial or remote admin over LoRa); DFU32 only scans |
| Flow | Arm, then wait (stage and verify first, then scan up to `arm_timeout_s`) |
| Update policy | Manual only, no polling |
| Staging partition | v1, and it is the primary store (flash, survives reboot). PSRAM holds buffers and logs. |
| Board safety rail | Configured `target.board` must be in `manifest.boards` |
| Target identity | Configured **app** MAC; DFU32 accepts MAC or MAC+1 that advertises the DFU UUID |
| BLE picker | `POST /api/v1/ble/scan` plus "Use as target" in the UI |
| API auth | Bearer token, claim on first use; hash stored in NVS |
| Provisioning | Improv Serial + Improv BLE (BLE only while unprovisioned) |
| Home Assistant | REST in v1; MQTT discovery in a later milestone |
| Remote access | LAN/subnet router only; MicroLink dropped |
| Artifact trust | HTTP + SHA-256 (signature can be added later) |
| Manifest | One manifest per target URL, one image |
| Web UI | Full config UI |
| Multi-target | Data model keyed by ID, v1 cap = 1 |
| Old bootloaders | Warn only |
| Retries | Configurable, default 3 full restarts with backoff |
| Hardware | Portable, 4 MB flash floor, **2 MB PSRAM required**; bench on Feather ESP32-S3 #5477 (4 MB/2 MB) (+ Feather V2) |
| WiFi during transfer | Option (`wifi_off_dfu` + per-request `wifi_off`); default set after bench A/B |
| Reference bootloader | OTAFIX 2.2 (`0.9.2-OTAFIX2.2-BP1.3`) on the production repeater |
| Logs | RAM ring + persisted run records (last 14–16) |
| License | MIT |

## Ranked open items

1. **B — Behaviour of the buttonless → bootloader jump with an ESP32 central.**
   - Source says: after writing `01 04` to the app's CP, the bootloader comes up at the **same** MAC with SD already initialised (GPREGRET 0xB1).
   - Unconfirmed:
     - whether the app's SoftDevice config (Bluefruit's MTU/bandwidth settings) carries over and limits the bootloader's MTU on this path;
     - whether DLE/2M survive the jump;
     - how long until the bootloader advertises;
     - whether the link must be dropped by DFU32 or by the target.
   - *Impact:* chunk size (20 vs 244 → about 5–10× throughput), timeouts, and the SCAN_BL step. This is the first thing M1 measures.
   - **Source hint (M1):** MeshCore's `startOTAUpdate` calls `Bluefruit.configPrphBandwidth(BANDWIDTH_MAX)` before `Bluefruit.begin()`. That configures the app's SoftDevice for MTU 247, and on the 0xB1 path the bootloader inherits the app's SoftDevice. 244-byte packets are therefore likely. Confirm with the M1 `probe` command (see `docs/BENCH.md`).
   - **Also found:** the app's OTA advertising has no DFU service UUID, so target matching on the exact app MAC is by address only (SPEC §5.2 updated).
   - **Bench result, nRF54 (XIAO nRF54LM20A, Feather V2):**
     - buttonless jump OK, taking about 4.5 s (link supervision timeout);
     - app MTU 92 (MeshCore `configPrphConn(92, …)`);
     - bootloader at the **same MAC** with **MTU 247**;
     - SYS_RESET returned the board to its app.

     This exposed an nRF54 bootloader class (device type 0x0054), now handled.
   - **Answered for nRF52840 (Heltec T096, OTAFIX 2.3):** MTU 247, so **244-byte packets** after the buttonless jump. The bootloader came up at **MAC+1** (peer data not accepted), which DFU32 handles. With its factory Heltec bootloader (0.9.0-dirty) the T096 never advertised after the jump; OTAFIX fixed that.
   - **Possible M2 tweak:** lower the supervision timeout from 4 s to about 2 s. That would cut the jump time by about 2 s and detect a dropped link mid-transfer sooner.

2. ~~**Q — Which bootloader is on your repeaters?**~~ **Answered:** the first repeater reports `0.9.2-OTAFIX2.2-BP1.3`.
   - OTAFIX 2.2 has MTU 247, DLE, lazy erase, +8 dBm, and BLE OTA recovery after a failure or power cut. That's the best case, and the spec's reference target.
   - Still to do: check any other repeaters. Ikoka's bootloader is in neither repo.
   - **New sub-item (B):** `get bootloader.ver` reads the UF2 string from flash. The BLE DIS Firmware Revision string that DFU32 reads is built differently (`<git ver> s140 <SD ver>`). M1 logs the DIS string so the classifier regex can be confirmed against real output.

3. ~~**B — Real `.bin` sizes and word alignment of your MeshCore builds.**~~ **Answered (M0):** MeshCore repeater v1.18.0 `.bin` sizes, all word-aligned:
   - RAK4631: 440 924 B
   - RAK3401: 410 036 B
   - XIAO: 360 768 B
   - your RAKSHARE build: 409 216 B

   That's about 55% of the app ceiling, so the brief's "~800 KB" was really the region limit, not the image size. The 832 KB staging partition has ample headroom. DFU32's CRC16 matches the `.dat` on all four images. The RAKSHARE UF2 payload is byte-identical to its `.bin` and loads at `0x26000` (S140 6.1.1 app start).

4. **B — Default for WiFi-off during the transfer.** **Resolved as an option:** config `wifi_off_dfu`, a per-request `wifi_off` override, and a UI checkbox, recorded in every run record. You'll compare on/off on the bench, and the shipped default comes from that. Your hypothesis is that nRF52 flash writes are the bottleneck, so it may not matter. Original notes:
   - The spec defaults to **true**, as you preferred: there is no live HTTP progress during the transfer; the LED and run record cover it.
   - On S3, software coexistence may make WiFi-on transfers reliable enough to keep live progress.
   - Plan: measure both in the M2 throughput matrix, then decide the default. Do you want the default fixed now regardless?

5. **A — App size fits 1.44 MB OTA slots on the 4 MB layout.**
   - Estimate: 1.0–1.2 MB for WiFi + NimBLE (central + peripheral) + httpd + mDNS + UI.
   - If it's over budget, fallbacks in order: (a) drop mDNS, (b) make Improv BLE a separate build flavour, (c) treat 4 MB as "no self-OTA" (one app slot), (d) raise the floor to 8 MB.
   - *Your call needed only if (c) or (d) becomes necessary.*

6. ~~**Q — Which Feather ESP32-S3, and its pins?**~~ **Answered:** the 4 MB flash / 2 MB PSRAM version (#5477). PSRAM is now required.
   - The pins are verified from Adafruit's arduino-esp32 and CircuitPython board files (SPEC §4).
   - Feather S3: NeoPixel GPIO33, power GPIO21, red LED GPIO13, BOOT GPIO0.
   - Feather V2: NeoPixel GPIO0, power GPIO2, red LED GPIO13, button GPIO38.
   - learn.adafruit.com isn't reachable from the build session, so the pinout page itself wasn't read.

7. **A — The DIS Firmware Revision string is available in the bootloader on both entry paths.**
   - DFU32 uses it to classify the bootloader and cross-check `sd_req` before START.
   - If it's missing on the 0xB1 path, classification falls back to `unknown` (warn), and the `SD_REQ_MISMATCH` precheck can only run on B2.
   - *Impact:* safety-rail coverage, not architecture.

8. **B — Erase time with the radio active (upstream).**
   - Source estimate is about 85–90 ms/page, so about 17–18 s for 800 KB. The START timeout is 60 s.
   - *Impact:* only timeout tuning.

9. **B — PRN default (8) and msys mbuf pool sizing.**
   - ESPHome claims a bootloader OOM above 8 unacknowledged packets. The MeshCore FAQ suggests 8–10.
   - *Impact:* throughput tuning. `CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT` will be tuned to whatever PRN × fragments needs.

10. **Q — A second `start ota` while already in OTA mode is unverified (it calls `Bluefruit.begin` twice).**
    - DFU32 doesn't care. But if your habit is to "run it again to be sure", we should check that it doesn't hang the repeater.
    - *Impact:* operator docs only.

11. **Q — Improv implementation.** The plan is a small in-tree implementation of the Improv Serial and BLE protocols, rather than depending on `improv-wifi/sdk-cpp`. I haven't checked its license or IDF fit yet. OK to decide during M3?

12. **A — No HTTPS in v1.**
    - `CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=n` saves roughly 100+ KB.
    - If your artifact server will only serve HTTPS (e.g. GitHub release URLs directly), this flips. `dfu32-meshcore-fetch` exists so your own server mirrors the artifacts over HTTP.

13. **A — The repeater app's BLE address is random static (the FICR default) and stable across reboots.**
    - The scan picker captures the type either way. If an app ever used a resolvable private address, MAC matching would break. Not the case for MeshCore, whose BLE is only on during OTA.

14. **Q (later) — Auto-trigger via wire.** You chose manual. If unattended flashing becomes interesting later, the options are UART to the repeater's `Serial1` (MeshCore doesn't use it today, so it would need firmware support) or USB host to its CDC port (needs an S3 USB-host-capable board and a cable).
