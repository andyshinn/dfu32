# Bench guide (M1)

This guide covers bench-testing M1 on your desk: build DFU32, stage an image, then probe and flash an nRF52 repeater over BLE.

**You need:**
- a Feather ESP32-S3 (or another supported board) on USB;
- the repeater on USB to your computer, for `start ota` and `ver`;
- the MeshCore DFU `.zip` for that repeater.

Ideally the repeater is a sacrificial board on the OTAFIX bootloader. `flash` erases its app, and an OTAFIX board recovers over BLE if a flash fails.

## 1. Build and flash DFU32

Activate your ESP-IDF 6.1 environment first. Then build:

```bash
idf.py -B build/feather_esp32s3 -DDFU32_BOARD=feather_esp32s3 build
```

Then flash and open the monitor (use your own port):

```bash
idf.py -B build/feather_esp32s3 -p /dev/cu.usbmodem1101 flash monitor
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

Tuning knobs:

| Option | Default | Use |
|---|---|---|
| `--prn N` | 8 | Packets per receipt. Try 4 if transfers stall. |
| `--packet N` | automatic | Cap the packet size, e.g. `--packet 20` to compare throughput. |
| `--interval MS` | 15–30 ms range | Request a fixed BLE connection interval, e.g. `--interval 8`. Shorter means more packets per second. `probe` takes it too, and both report the interval actually negotiated. |
| `--settle MS` | 500 | Delay between START and the size packet. |
| `--wait S` | 600 | How long to wait for the target. |

## Bench results so far

| Date | DFU32 | Target / bootloader | Image | Settings | Stream | Total |
|---|---|---|---|---|---|---|
| 2026-11-04 | Feather V2 (1M PHY) | Heltec T096, OTAFIX 2.3 (MAC+1 after jump) | 413,680 B MeshCore repeater | 244 B, PRN 8, default interval | 44.9 s (9.2 kB/s) | 53.0 s, `ver` OK |

Each 8-packet window took about 212 ms. That points at connection events, not nRF flash speed. The levers are `--prn`, `--interval`, and 2M PHY (Feather S3).

## If `probe` can't find the bootloader after the jump

DFU32 logs every DFU-looking advertiser it saw, by name, legacy DFU UUID or Secure DFU UUID. Then:
- If nothing was seen, run `scan 10` right away and look for a `*DFU*` name or anything near the repeater's MAC. On the repeater, `get bootloader.ver` (before probing) tells us which bootloader it runs.
- If the board is stuck in its bootloader (on OTAFIX and older upstream it advertises forever), run `blreset` or power-cycle it. Nothing has been erased.
- `Secure DFU FE59` means the board's bootloader uses Nordic Secure DFU, which DFU32 does not support.

## If a flash fails

- After a failure DFU32 sends SYS_RESET. An OTAFIX bootloader then comes back in BLE OTA mode, usually at **MAC+1**, and DFU32 finds it there. Run `flash --yes` again; no `start ota` is needed, because the app is gone and the bootloader is waiting.
- If START reports `INVALID_STATE`, run `blreset`, then `flash --yes`.
- If the board is in USB DFU (upstream bootloader ≤ 0.9.x, or after a power cut), double-tap reset and copy the MeshCore `.uf2` to the drive that appears.
