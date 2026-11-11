# Recovery

What to do when a repeater or DFU32 itself is left in a bad state. Start with the run record: **Runs** in the web UI, or `runs` / `run N` on the console. It says what failed, whether the repeater's app was erased (`target_erased`), and has the log.

## The repeater

### The flash failed and `target_erased` is `no`

Nothing was written. The repeater rebooted into its app (DFU32 sent SYS_RESET), or it reboots by itself when its OTA window ends. Fix the cause the error names (board, hash, SoftDevice requirement, not found) and flash again; it needs `start ota` again.

### The flash failed and `target_erased` is `yes`

The repeater's app is gone, but its bootloader is waiting in BLE OTA mode, usually at the **app MAC + 1** (OTAFIX and upstream bootloader ≥ 0.10). This is the normal recovery path, and it doesn't need `start ota`:

1. Fix the cause if the error names one (for example a link that kept dropping: move DFU32 closer).
2. **Flash** with *Flash from: staged image* (API: `{"source":"staged"}`; console: `flash --yes`). DFU32 finds the bootloader at MAC or MAC+1 and starts again.

If START answers `INVALID_STATE`, the bootloader still holds the half-finished session: run `blreset` on the console (or power-cycle the repeater), then flash again.

### DFU32 can't find the erased repeater

- The repeater lost power: power it again. An OTAFIX bootloader comes back in BLE OTA mode with the app erased.
- The run record says `TARGET_LOST_USB_RECOVERY_REQUIRED`, or the bootloader is upstream ≤ 0.9.x: these fall back to **USB DFU** after a failed transfer. Connect the repeater over USB, double-tap reset to get the `…BOOT` drive, and copy the MeshCore `.uf2` for the board onto it.
- `scan` on the console (or **Find the repeater**) lists DFU-looking advertisers. A `Secure DFU FE59` entry means a Nordic Secure DFU bootloader, which DFU32 doesn't support: use USB.

### The repeater is stuck in its bootloader but its app is fine

That happens after `probe --no-reset`, or an abort before START. Run `blreset` (sends SYS_RESET) or power-cycle it.

## DFU32

### After a firmware update it came back on the old version

That's the rollback working: the new firmware failed its self-check, couldn't start the HTTP API, or showed no WiFi activity at all within 90 seconds. The console log of the failed boot says why. Install a fixed build; the device keeps working on the old one meanwhile.

A network problem outside DFU32 doesn't cause a rollback: if the router is down during the update (the radio reports "AP not found") or no network is stored, the new firmware is kept, and it joins once the network is back.

### It doesn't come back at all

Connect it over USB and look at the console (`idf.py -p <port> monitor`). Then:

- **Reflash just the app**, which keeps WiFi, the token, the target, the staged image and the runs:

  ```bash
  python -m esptool --port <port> write-flash 0x20000 dfu32.bin
  ```

  The offset is `0x20000` on the 4 MB layout (Feather S3) and `0x30000` on the 8 MB layout (Feather V2, N16R8, XIAO ESP32S3, XIAO ESP32C5). If the board was running from its second slot after a self-update, also write `ota_data_initial.bin` (from the same build) at `0xF000` (4 MB) or `0x18000` (8 MB) so it boots the first slot.

- **Fresh install** with the `-merged.bin` at `0x0`. The merged image fills the gaps between its parts, so it erases WiFi, the token, the target and the run history. The staged image lies beyond the app slots and survives.

### Lost the API token

Hold the button 5–15 seconds or run `token clear` on the console: either turns the token off, so the web UI and API are open again (**System → Access → Require a token** sets a new one). `token rotate` on the console prints a new token instead. Boards without a button: the console.

### WiFi details wrong or the network changed

Hold the button 2–5 seconds and set WiFi over Improv BLE, or use `wifi <ssid> <password>` on the console. Note that DFU32 joins WPA2-Personal networks (and WPA2/WPA3 mixed ones), not WPA3-only.

### Start over

Hold the button 15 seconds or more: DFU32 erases its settings and WiFi and reboots into Improv. The staged image and run history stay. It is refused while a flash or console operation is running; abort that first.
