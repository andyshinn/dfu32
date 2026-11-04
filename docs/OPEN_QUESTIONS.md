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
| Staging partition | v1, and it is the primary store; PSRAM is not used for staging |
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
| Hardware | Portable, 4 MB floor; bench on Feather ESP32-S3 (+ Feather V2) |
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

2. **Q — Which bootloader is on each of your repeaters right now?**
   - Run `get bootloader.ver` on each repeater, or check the DFU32 IDENTIFY log once M1 exists.
   - With "warn only" chosen, upstream ≤0.9.2 boards will need **USB to recover** after any failed transfer or power cut.
   - *Impact:* whether you should upgrade them to OTAFIX before deploying DFU32. This is strongly recommended, especially for RAK3401, which is OTAFIX-only anyway. Also: Ikoka's bootloader is in **neither** repo, so I couldn't check it.

3. **B — Real `.bin` sizes and word alignment of your MeshCore builds.**
   - Release zips are 353–431 KB, so the bins are probably 450–700 KB, below the 798 720 / 802 816 B limits.
   - A bin whose size isn't a multiple of 4 is refused (DFU32 doesn't pad).
   - *Impact:* staging size, and whether the 4 MB layout's 832 KB staging is comfortable.

4. **Q — Default for `wifi_off_during_dfu`.**
   - The spec defaults to **true**, as you preferred: there is no live HTTP progress during the transfer; the LED and run record cover it.
   - On S3, software coexistence may make WiFi-on transfers reliable enough to keep live progress.
   - Plan: measure both in the M2 throughput matrix, then decide the default. Do you want the default fixed now regardless?

5. **A — App size fits 1.44 MB OTA slots on the 4 MB layout.**
   - Estimate: 1.0–1.2 MB for WiFi + NimBLE (central + peripheral) + httpd + mDNS + UI.
   - If it's over budget, fallbacks in order: (a) drop mDNS, (b) make Improv BLE a separate build flavour, (c) treat 4 MB as "no self-OTA" (one app slot), (d) raise the floor to 8 MB.
   - *Your call needed only if (c) or (d) becomes necessary.*

6. **Q — Is the Feather ESP32-S3 you have the 4 MB flash / 2 MB PSRAM version (#5477) or the 8 MB / no PSRAM version (#5323)?**
   - Also confirm the NeoPixel and button pins in §4. They're from memory and marked *(verify)*.
   - *Impact:* bench profile only.

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
