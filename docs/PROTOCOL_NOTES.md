# Protocol notes: verified facts and citations

These are the ground-truth findings behind [`SPEC.md`](../SPEC.md), gathered from the reference sources on 2026-11-04. Paths are relative to each repo at the commit listed.

| Short | Repo | Commit / tag |
|---|---|---|
| **U** | adafruit/Adafruit_nRF52_Bootloader | `c67f0bc` = 0.11.0 |
| **O** | oltaco/Adafruit_nRF52_Bootloader_OTAFIX | `3a75482` = 0.9.2-OTAFIX2.3-BP1.4. Based on upstream `a48e944` (0.9.2 + 12 commits); it does **not** include upstream 0.10 or 0.11. |
| **NDP** | lucidnx/nrf_dfu_py | `8289b32` (no license) |
| **ESPH** | esphome/esphome PR 20481 (`adafruit_ble/dfu.py`) | head `28356aa` |
| **WASP** | wasp-os/ota-dfu-python | HEAD |
| **NRFU** | adafruit/Adafruit_nRF52_nrfutil | HEAD |
| **MC** | meshcore-dev/MeshCore | `81dbae5` = repeater-v1.18.0 |
| **BF** | adafruit/Adafruit_nRF52_Arduino (`Bluefruit52Lib/src/services/BLEDfu.cpp`) | HEAD (identical in MeshCore's pinned core fork `d541301`) |
| **NIM** | h2zero/esp-nimble-cpp | tag 2.6.0 |

Throughout, `T` = `lib/sdk11/components/libraries/bootloader_dfu/dfu_transport_ble.c` and `SB` = `…/dfu_single_bank.c`.

## Corrections to the original brief

| Brief said | Source says |
|---|---|
| `start ota` on a repeater sets GPREGRET=0xA8 and resets | **No.** MC `NRF52Board.cpp:412-458` starts Bluefruit `BLEDfu` ("buttonless") in the running app. It advertises `<BOARD>_OTA` at the **app MAC** and replies `OK - mac: …`. The jump only happens when a DFU client writes START to the app's CP: BF `:107-182` saves peer data, sets GPREGRET=**0xB1** and jumps with no reset. |
| Bootloader is at app MAC + 1 | **Only on the 0xA8 reset path** (T U:1106-1115 / O:1215-1225: `addr[0] += 1`, the LSB, uint8 wrap). On the 0xB1 app-jump path with peer data, it keeps the **same** address (T U:1099-1102). After a failed OTAFIX transfer, the board resets with 0xA8, so it is at MAC+1. |
| Name `AdaDFU` | Upstream: `AdaDFU` (T:44-47). OTAFIX: per board, e.g. `4631_DFU`, `3401_DFU`, `XIAO_DFU` (`board.mk`). |
| Legacy clients write 20-byte packets | U ≥0.10 and OTAFIX configure ATT MTU 247 with a 244-byte Packet char. U ≤0.9.2 uses MTU 23 and 20 bytes. Neither side starts the MTU exchange, so the client must. **Every data write must be a multiple of 4** (T:445-453). |
| Image ~800 KB | The nRF52840 maximum app size is **802 816 B** (S140 6.1.1, app at 0x26000) or **798 720 B** (S140 7.3.0, XIAO). The actual MeshCore repeater v1.18.0 `.bin` files are **360–441 KB** (measured in M0). The 0x26000 app start for S140 6.1.1 is confirmed from a RAK UF2's block addresses. |
| Erase on INIT | Erase happens when the 12-byte size packet after START arrives. `10 01 01` is sent after the erase completes (SB:133-160, T:285-297). OTAFIX erases lazily, only the first page before replying. |
| MeshCore companion BLE trigger PR #2078 | Closed unmerged 2026-07-28. Replaced by #2323, which adds BLEDfu to the companion with MITM security. Not relevant to repeaters. |
| Primary reference nrf_dfu_py | It has **no license**. Use it for behaviour only. Port from ESPH (MIT), WASP (Apache-2.0) or NRFU (BSD-3). |

## Bootloader behaviour by variant

| | U ≤0.9.2 | U 0.10+ | OTAFIX |
|---|---|---|---|
| ATT MTU / Packet max | 23 / 20 | 247 / 244 | 247 / 244 |
| Packet props | WNR | W + WNR | WNR |
| DLE | — | on | on (requests 251) |
| Erase at START | full image size, then reply | full image size, then reply | first page only (lazy) |
| BLE DFU inactivity timeout | none (advertises forever) | 360 s (`dfu_bank_internal.h:43`) | none |
| No-app boot | USB DFU | USB DFU (`DEFAULT_TO_OTA_DFU` off) | **BLE OTA** (O `main.c:289-291`) |
| SYS_RESET mid-transfer | reset, then no app, then **USB** | back to BLE OTA (SB:604-618, M:234-236) | 0xA8 reset, then BLE OTA |
| Power cut mid-transfer | USB DFU | USB DFU | BLE OTA (MAC+1) |
| TX power (840) | +4 dBm | +4 dBm | +8 dBm |
| Small-packet accumulator | — | — | if the first packet is ≤64 B, flushes are padded with 0xFF. Non-multiple-of-4 packets would corrupt the image, so always send multiples of 4. |

- All shipping builds are **single bank** (`DUALBANK_FW` off; dual bank caps the app at 401 408 B anyway).
- After START is answered, the settings mark bank 0 INVALID. A partial image never boots.
- A disconnect keeps the DFU state and re-advertises (T U:807-828). A new START then returns INVALID_STATE because START needs IDLE (SB:370 / O SB:333).
- No resume is supported. OTAFIX's README says "Disable resume".

## Init packet (`.dat`, dfu-ver 0.5, LE)

| Off | Field | Check (U `src/dfu_init.c:216-246`, O :113-187) |
|---|---|---|
| 0 | u16 device_type | must be **0x0052**, else FORBIDDEN → `10 02 06` |
| 2 | u16 device_rev | checked only for SD/BL updates |
| 4 | u32 app_version | not checked (MeshCore leaves it at 0xFFFFFFFF) |
| 8 | u16 N | must be ≥1 |
| 10 | u16 sd_req[N] | must contain 0xFFFE or the installed FWID |
| 10+2N | u16 CRC16-CCITT (init 0xFFFF) of the `.bin` | compared at VALIDATE → `10 04 05` |

- Each Packet write during INIT is zero-padded to 4 bytes separately (T:396-416), so send the `.dat` in **one** write.
- MeshCore builds use `adafruit-nrfutil dfu genpkg --dev-type 0x0052 --sd-req <fwid>`. The fwid is 0x00B6 for RAK4631/RAK3401 (S140 6.1.1) and 0x0123 for XIAO (S140 7.3.0).

## Host implementation behaviour (for tuning defaults)

| | Delay START→size | START timeout | Chunk | PRN | PRN timeout | Retries |
|---|---|---|---|---|---|---|
| NDP | 0.4 s (README: 0.6 s if it times out) | 60 s | min(mtu−3, 244), **not** 4-aligned | 8 | 5 s, then continue | 3 × full restart, 3 s apart |
| ESPH | 0.6 s | 60 s | max(20, min(mtu−3, 244)), rounded down to a multiple of 4 | 8 ("BL OOM beyond 8") | 2 s, fatal | 3; START INVALID_STATE → `06` and retry |
| WASP | none | 30 s | 20 | 5 | — | none |
| NRFU | none | 10 s (too short) | 20 | 10 | — | resend START once |

- If a Packet write arrives before the START event is processed, it is silently dropped (T:558-562). That is why the delay matters.
- An erase estimate from NRFU serial is about 89.7 ms per 4 KB page, so about 18 s for 800 KB. That is why the START timeout is 60 s.
- No host checks the byte count in the PRN. DFU32 does.

## esp-nimble-cpp gotchas (2.6.0)

- `writeValue(data, len, false)` with `len ≤ mtu−3` makes a single `ble_gattc_write_no_rsp_flat` call. It returns `false` on any rc, including `BLE_HS_ENOMEM`, and the rc is **not** stored in `getLastError()`.
- With `len > mtu−3` it **silently switches to a long write with response** (`NimBLERemoteValueAttribute.cpp:46-50`).
- `NimBLEDevice::deinit()` and re-init are supported (2.6.0 fixed reinit crashes). Never call `esp_bt_controller_mem_release`.
- `connect(addr, deleteAttrs, async, exchangeMTU=true)` sends the MTU request automatically. `setDataLen` and `updatePhy` are available on the client.

## MeshCore serial CLI (repeater)

- The CLI is on TinyUSB CDC (`Serial`) and is not on the hardware UART.
- It ends lines on CR, echoes input, and prefixes replies with `"  -> "`. There is no password.
- `start ota` is also allowed remotely after an admin login over LoRa (MC `CommonCLI.cpp:220`; `docs/faq.md:732-747`).
- `ver` → `vX.Y.Z-<sha7> (Build: DD-Mon-YYYY)`; `board` → e.g. `RAK 4631`; `get bootloader.ver` scans flash for `"UF2 Bootloader "`.
