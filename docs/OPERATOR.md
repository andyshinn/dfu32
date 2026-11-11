# Operator guide

This is the everyday path: one DFU32 next to a MeshCore repeater, firmware served from your own computer or server, and updates done from the web UI. The [bench guide](BENCH.md) covers testing and the console. [RECOVERY.md](RECOVERY.md) covers what to do when something goes wrong.

## 1. Put DFU32 on your network

If you installed DFU32 from the installer page ([dfu32.app](https://dfu32.app/)), the page asks for your network right after installing and then links to the device's web UI: skip to §2. To change the network later the same way, connect the board over USB, click the install button and choose **Change Wi-Fi**.

Unlike Improv BLE, which only opens while no network is stored or after a button press, Improv Serial is always on: anyone who can plug into DFU32's USB port can change its WiFi network, just as they could with the `wifi` console command. Treat the USB port as physical access, and don't leave it connected to a machine you don't trust.

Otherwise, a DFU32 with no WiFi stored opens **Improv BLE** by itself; its NeoPixel blinks white slowly.

1. Open the Home Assistant companion app (Settings → Devices → Discovered), or https://www.improv-wifi.com/ in desktop Chrome or Edge.
2. Pick **DFU32** (or `dfu32-xxxx`) and enter your network's name and passphrase. DFU32 joins **WPA2-Personal** and open networks; a WPA2/WPA3 mixed network works, a WPA3-only one doesn't.
3. When it has joined, the client offers a link to `http://<ip>/`. That opens the web UI.

To change the network later, hold the button for 2–5 seconds (the NeoPixel blinks amber slowly while held); Improv opens again for 5 minutes. Over USB, `wifi <ssid> <password>` on the console does the same.

## 2. API token (optional)

Out of the box DFU32 needs no token: the web UI and the API are open to anything that can reach it on your network. For a device that stays on your LAN, that is usually fine.

To lock it down, open **System → Access → Require a token**. The page shows the token once so you can copy it for scripts or Home Assistant; this browser keeps it, and any other browser signs in once with it. **Turn off token** in the same place makes it open again.

Lost the token? Hold the button 5–15 seconds (amber fast blink), or run `token clear` on the console: both turn the token off. `token rotate` prints a new one instead.

## 3. Serve the firmware (optional)

You can skip this section: **Image** in the web UI takes a DFU `.zip` (such as a MeshCore release asset) straight from your browser (§5). A server is for when DFU32 should fetch images itself, so a release can be staged without anyone uploading it.

On the computer that will host the firmware, package a MeshCore release:

```bash
python3 tools/dfu32_pack.py RAK_4631_repeater-v1.18.0-81dbae5.zip --board rak4631 --out site/rak4631
```

```bash
python3 -m http.server -d site 8000
```

The `--board` name is yours to choose. It must match the board you give the target in the next step, and it stops a RAK image from ever going to a Heltec. Any plain-HTTP server works; HTTPS isn't supported in v1.

## 4. Set up the target

In the web UI, open **Target**:

1. On the repeater, run `start ota` (over USB, or remotely: see §6). Its reply includes its MAC, and it starts advertising for a few minutes.
2. In **Find the repeater**, press **Scan**. Your repeater shows as *app buttonless*. Press **Use as target**.
3. Optionally fill in an id (for example `attic-repeater`; blank means `target`), a name, and the board name. With a server (§3), add the manifest URL (`http://<computer>:8000/rak4631/manifest.json`) and the board name from §3. Then **Save**. Only the MAC is needed to flash an uploaded image.

The scan put the repeater into OTA mode. Reboot it (or wait for it to time out) before you flash, so the flash below starts from a repeater in normal operation.

## 5. Update a repeater

First get the image onto DFU32 (it is **staged**: kept in DFU32's flash until replaced):

- **Upload it:** on **Image**, choose (or drop) the DFU `.zip`. Your browser unpacks it and checks it, you confirm the version (taken from a MeshCore file name), and **Stage** sends it to DFU32, which checks it again and writes it. The image isn't tied to the target. Without a board on it, DFU32 can't tell if it suits the repeater, so pick the right zip (or fill in **Board**, which DFU32 then compares with the target's). From a script: `python3 tools/dfu32_stage_image.py <zip> --upload dfu32-xxxx.local` (add `--token <token>` if DFU32 requires one).
- **Or over USB**, with no network: on [dfu32.app](https://dfu32.app/), **Connect device**, **Stage nRF52 image**, pick the zip.
- **Or from your server** (§3), on **Status**: **Check** fetches the manifest and shows the version, whether it suits the board, and whether it is newer than what DFU32 last flashed (it changes nothing). **Stage** downloads and verifies the image. Flashing with *Flash from: manifest* does both itself.

**Image** shows what is staged, re-checks it (**Verify**) and clears it. Then, on **Status**:

1. *Flash from: staged image* (the only choice when the target has no manifest URL), or *manifest* to fetch first.
2. **Flash** arms DFU32: it checks the image and waits up to 10 minutes for the repeater.
3. Run `start ota` on the repeater (§6). DFU32 sees it, switches it into its bootloader and sends the image. The progress bar fills; the NeoPixel goes from cyan blinking to magenta getting brighter.
4. **Succeeded** means the repeater has rebooted into the new firmware. Its version shows as **Last flashed**. Run `ver` on the repeater to confirm.

Things to know:

- Nothing on the repeater is touched until DFU32 has seen it and checked the image against its bootloader. A wrong board, a bad hash or a SoftDevice mismatch stops before anything is erased.
- **Abort** before the transfer leaves the repeater as it was. During the transfer it leaves the repeater's app erased: the repeater waits in its bootloader, and **Flash** with *Flash from: staged image* finishes the job (no `start ota` needed).
- **WiFi off during the transfer** makes the transfer about 20% faster but the page goes quiet until it ends. It's off by default.
- Every flash is recorded under **Runs**, with its log.

## 6. `start ota` over LoRa

The repeater doesn't need to be on your desk. From the MeshCore companion app, log in to the repeater as admin and send `start ota` as a remote CLI command. The reply carries the MAC. DFU32 must be within BLE range of the repeater (a few metres indoors, more with line of sight).

## 7. Update DFU32 itself

**System → Firmware update**: choose `dfu32.bin` for your board (from a CI build or release, not the `-merged.bin`) and **Install**. DFU32 checks that the file is DFU32 for the same chip and board, writes it to its spare slot, and reboots into it.

If the new firmware can't bring WiFi and the web UI back within 90 seconds, DFU32 goes back to the previous firmware by itself. The page tells you which version came back.

## 8. Lights and the button

| NeoPixel | Meaning |
|---|---|
| dim green | idle |
| dim yellow, slow blink | WiFi configured but not connected |
| white, slow blink | Improv open, waiting for WiFi settings |
| blue pulse | fetching or checking an image |
| cyan blink | armed: run `start ota` on the repeater |
| magenta, getting brighter | transfer in progress |
| green, 10 s | success |
| solid red | the last job failed |
| red, fast blink | the repeater's app is erased: flash it again |

The plain LED (red on the Feathers, blue on the RAK11200, orange on the XIAO ESP32S3, yellow on the XIAO ESP32C5) stays on while a repeater is known to be erased, and blinks during a transfer.

Hold the button and let go: 2–5 s opens Improv for 5 minutes, 5–15 s clears the API token (open access), 15 s or more erases DFU32's settings and WiFi (not the staged image or the run history). The factory reset is refused while a flash is running.
