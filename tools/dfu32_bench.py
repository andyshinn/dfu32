#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the DFU32 bench checklist (docs/BENCH.md §7) against real hardware.

The script drives DFU32's USB-serial console and its HTTP API, serves the
firmware (and broken variants of it) over HTTP itself, flashes DFU32 with
esptool where a check needs a different build, and stops to ask you only for
the things it can't do: sending `start ota` to the repeater (unless
--repeater-port is given).

Everything DFU32 prints goes to serial.log, every API call to http.log, and
the results to report.md / report.json, in a new folder under bench-results/.
Passwords and tokens are redacted from all of them.

Examples:
  # everything, on a Feather V2 that already runs the current build
  dfu32_bench.py --port /dev/cu.usbserial-575E0522651 --zip RAK_4631_repeater-v1.18.0.zip \\
      --board heltec_t096 --mac F4:1C:36:A7:33:95 --wifi-ssid MyNet --wifi-pass secret \\
      --debug-app-bin dfu32-feather_esp32_v2-debug.bin --app-bin dfu32.bin

  # only what unblocks M4 (claim, target, LoRa happy path, WiFi on/off comparison)
  dfu32_bench.py --port ... --zip ... --board heltec_t096 --mac ... --minimum

  # the M6 soak: 20 flashes alternating two versions, with faults; --repeater-port sends `start ota`
  dfu32_bench.py --port ... --repeater-port ... --unattended --zip v1.18.0.zip --soak-zip v1.17.0.zip \
      --board heltec_t096 --soak 20 --debug-app-bin dfu32-feather_esp32_v2-debug.bin --app-bin dfu32.bin

  dfu32_bench.py --list
"""
from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench import checks  # noqa: E402  (also registers the checks)
from bench.console import Redactor, SerialConsole  # noqa: E402
from bench.flasher import Flasher  # noqa: E402
from bench.runner import Ctx, ordered, run_checks  # noqa: E402
from bench.site import FirmwareSite  # noqa: E402

MINIMUM = ["3.1", "3.2", "3.5", "5.1"]


def matches(spec: str, tid: str) -> bool:
    """'2.5' selects 2.5a-2.5d; '3' selects section 3; '3.1' selects only 3.1 (not 3.10)."""
    spec = spec.strip()
    if not spec:
        return False
    if tid == spec:
        return True
    if "." not in spec:
        return tid.split(".")[0] == spec
    return tid.startswith(spec) and tid[len(spec):].isalpha()


def select(args) -> list:
    tests = ordered()
    if args.only:
        specs = args.only.split(",")
    elif args.minimum:
        specs = MINIMUM
    elif args.sections:
        specs = args.sections.split(",")
    elif args.soak:
        specs = ["6"]  # --soak on its own runs just the soak
    else:
        specs = None
        tests = [t for t in tests if t.section != "6"]  # the soak is opt-in: --soak N
    if specs is not None:
        tests = [t for t in tests if any(matches(s, t.id) for s in specs)]
    if args.skip:
        tests = [t for t in tests if not any(matches(s, t.id) for s in args.skip.split(","))]
    return tests


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_argument_group("hardware")
    g.add_argument("--port", help="DFU32's serial port, e.g. /dev/cu.usbserial-575E0522651")
    g.add_argument("--baud", type=int, default=115200, help="console baud rate (default 115200)")
    g.add_argument("--mac", help="the repeater's BLE MAC (from `start ota`); default: the one stored on DFU32")
    g.add_argument("--addr-type", choices=["random", "public"], default="random")
    g.add_argument("--target-id", default="t096", help="API target id (default t096)")
    g.add_argument("--repeater-port", help="optional: the repeater's USB console, to send `start ota` for you "
                                           "(the LoRa happy path 3.5 still asks you to use the companion app)")
    g.add_argument("--repeater-baud", type=int, default=115200)

    g = ap.add_argument_group("firmware under test (the nRF52 image)")
    g.add_argument("--zip", type=Path, help="adafruit-nrfutil DFU .zip")
    g.add_argument("--bin", type=Path, help="application .bin (instead of --zip)")
    g.add_argument("--dat", type=Path, help="init packet .dat (instead of --zip)")
    g.add_argument("--board", help="board id, as in `dfu32_pack.py --board` (e.g. heltec_t096)")
    g.add_argument("--version", default="", help="version string (default: the zip's file name)")

    g = ap.add_argument_group("network")
    g.add_argument("--wifi-ssid", help="network for `wifi <ssid> <pass>` (needed after a merged-bin flash)")
    g.add_argument("--wifi-pass", default=os.environ.get("DFU32_WIFI_PASS"),
                   help="WiFi password (or set DFU32_WIFI_PASS)")
    g.add_argument("--token", default=os.environ.get("DFU32_TOKEN"),
                   help="existing API token (or DFU32_TOKEN); otherwise the script gets one from the console")
    g.add_argument("--device-ip", help="DFU32's IP (default: read from `wifi` on the console)")
    g.add_argument("--host-ip", help="this computer's LAN IP as DFU32 should reach it (default: auto)")
    g.add_argument("--site-port", type=int, default=8000, help="port for the built-in firmware server (8000)")

    g = ap.add_argument_group("flashing DFU32 itself (esptool)")
    g.add_argument("--merged-bin", type=Path, help="flash this merged image at 0x0 first (wipes NVS: WiFi, token, "
                                                   "target)")
    g.add_argument("--erase-flash", action="store_true", help="erase the whole flash before --merged-bin")
    g.add_argument("--app-bin", type=Path, help="the normal app-only dfu32.bin: flashed first when there's no "
                                                "--merged-bin, and put back after section 4")
    g.add_argument("--debug-app-bin", type=Path, help="app-only build with CONFIG_DFU32_DEBUG_ENDPOINTS=y, "
                                                      "flashed for section 4")
    g.add_argument("--layout", choices=["4mb", "8mb"], help="flash layout (default: from the boot log)")
    g.add_argument("--flash-baud", type=int, default=460800, help="esptool baud rate (try 115200 if it fails)")
    g.add_argument("--esptool", default="", help="esptool command (default: python -m esptool)")

    g = ap.add_argument_group("what to run")
    g.add_argument("--list", action="store_true", help="list the checks and exit")
    g.add_argument("--only", help="comma-separated check ids, e.g. 3.1,3.5 or 2.5 (all of 2.5a-d)")
    g.add_argument("--sections", help="comma-separated sections, e.g. 1,3")
    g.add_argument("--skip", help="comma-separated check ids to leave out")
    g.add_argument("--minimum", action="store_true", help="only what unblocks M4: " + ", ".join(MINIMUM))
    g.add_argument("--compare-runs", type=int, default=3, help="flashes per WiFi setting in 5.1 (default 3)")
    g.add_argument("--start-ota-timeout", type=int, default=0,
                   help="how long DFU32 waits for `start ota` each time (default 300 s, or 120 s with "
                        "--repeater-port, where the runner sends it at once)")

    g = ap.add_argument_group("soak (section 6, M6): runs only with --soak")
    g.add_argument("--soak", type=int, default=0, metavar="N",
                   help="flash N times in a row (M6 asks for 20); on its own, runs only the soak")
    g.add_argument("--soak-zip", type=Path, help="a second DFU .zip for the same board: runs alternate between it "
                                                 "and --zip, so each flash visibly changes the version")
    g.add_argument("--soak-bin", type=Path, help="second image's .bin (instead of --soak-zip)")
    g.add_argument("--soak-dat", type=Path, help="second image's .dat (instead of --soak-zip)")
    g.add_argument("--soak-version", default="", help="second image's version (default: its zip's file name)")
    g.add_argument("--soak-faults", type=soak_faults, default=list(checks.SOAK_FAULTS),
                   help=f"comma-separated faults to mix in, taken in turn (default {','.join(checks.SOAK_FAULTS)}; "
                        "'none' for clean runs only). deadline, stall and restore need the debug build "
                        "(--debug-app-bin) and are left out without it")
    g.add_argument("--soak-fault-every", type=int, default=3, metavar="K",
                   help="inject a fault on every K-th run (default 3)")

    g = ap.add_argument_group("behaviour")
    g.add_argument("--unattended", action="store_true",
                   help="run with nobody at the keyboard: 3.5 sends `start ota` over --repeater-port instead of "
                        "LoRa (noted in the report), and checks that would prompt you are skipped")
    g.add_argument("--manual-reset", action="store_true", help="ask you to press RESET instead of pulsing RTS")
    g.add_argument("--power-cycle", action="store_true", help="1.4: ask you to unplug DFU32 instead of a reset")
    g.add_argument("--keep-hostname", action="store_true", help="3.13: leave the hostname as bench-dfu")
    g.add_argument("--no-reset", action="store_true", help="don't reset DFU32 when the run starts")
    g.add_argument("--eol", choices=["cr", "lf", "crlf"], default="cr", help="line ending sent to the console")
    g.add_argument("--echo", action="store_true", help="also print everything DFU32 logs")
    g.add_argument("--out", type=Path, help="results folder (default bench-results/<date-time>)")
    return ap


def soak_faults(text: str) -> list:
    items = [f.strip() for f in text.split(",") if f.strip()]
    if items == ["none"]:
        return []
    bad = [f for f in items if f not in checks.SOAK_FAULTS]
    if bad or not items:
        raise argparse.ArgumentTypeError(f"unknown fault(s) {', '.join(bad) or text!r}; "
                                         f"choose from {', '.join(checks.SOAK_FAULTS)} or 'none'")
    return items


def setup(ctx: Ctx) -> None:
    a = ctx.args
    if a.merged_bin:
        print(f"\nflashing the merged image {a.merged_bin} at 0x0", flush=True)
        ctx.flash_images([(0, a.merged_bin)], erase_all=a.erase_flash)
    elif a.app_bin:
        if not ctx.layout:
            ctx.reset_and_wait()
        lay = ctx.need_layout()
        print(f"\nflashing the app {a.app_bin} at 0x{lay.app:X} (keeps WiFi, token and target)", flush=True)
        ctx.flash_images([(lay.app, a.app_bin)])
    elif not a.no_reset:
        ctx.reset_and_wait()
    else:
        ctx.con.sync()
    rows = ctx.runs()
    ctx.first_seq = (max(r["seq"] for r in rows) + 1) if rows else 1
    print(f"DFU32 {ctx.fw_version or '(version not seen: --no-reset)'}, layout "
          f"{ctx.layout.name if ctx.layout else 'unknown'}, {len(rows)} run record(s) on the device", flush=True)


def main(argv: list[str] | None = None) -> int:
    ap = build_parser()
    args = ap.parse_args(argv)
    if args.list:
        for t in ordered():
            extra = f"  [{', '.join(t.needs)}]" if t.needs else ""
            print(f"{t.id:6} {t.title}{extra}")
        print("\n[repeater] = needs a `start ota`.\n"
              "3.3 runs right after 3.5, so the happy path really waits for a LoRa `start ota`.")
        return 0
    for need in ("port", "board"):
        if not getattr(args, need):
            ap.error(f"--{need} is required")
    if not (args.zip or (args.bin and args.dat)):
        ap.error("give the repeater firmware: --zip, or --bin and --dat")
    if not args.start_ota_timeout:
        args.start_ota_timeout = 120 if args.repeater_port else 300
    if args.unattended and not args.repeater_port:
        ap.error("--unattended needs --repeater-port, so the runner can send `start ota` itself")
    if args.unattended and (args.manual_reset or args.power_cycle):
        ap.error("--unattended can't be combined with --manual-reset or --power-cycle")
    if bool(args.soak_bin) != bool(args.soak_dat):
        ap.error("--soak-bin and --soak-dat go together")
    if args.soak < 0 or args.soak_fault_every < 0:
        ap.error("--soak and --soak-fault-every can't be negative")
    if args.debug_app_bin and not args.app_bin:
        print("note: without --app-bin the debug build stays on DFU32 after section 4", file=sys.stderr)

    selected = select(args)
    if not selected:
        ap.error("no checks selected")
    try:
        site = FirmwareSite.from_package(args.zip, args.bin, args.dat, board=args.board, version=args.version,
                                         port=args.site_port)
    except (ValueError, KeyError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if args.soak_zip or args.soak_bin:
        try:
            site.add_package("alt", args.soak_zip, args.soak_bin, args.soak_dat, args.soak_version)
        except (ValueError, KeyError, OSError) as e:
            print(f"error: the soak's second image: {e}", file=sys.stderr)
            return 2

    out = args.out or Path("bench-results") / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    redactor = Redactor()
    for name, secret in (("--wifi-pass", args.wifi_pass), ("--token", args.token)):
        if secret and not redactor.add(secret):
            print(f"warning: {name} is shorter than {Redactor.MIN_LEN} characters, so it is NOT redacted from the "
                  "logs (masking it would mangle every line)", file=sys.stderr)
    eol = {"cr": "\r", "lf": "\n", "crlf": "\r\n"}[args.eol]
    echo = (lambda line: print(f"      | {line}", flush=True)) if args.echo else None
    con = SerialConsole(args.port, args.baud, log_path=out / "serial.log", eol=eol, redactor=redactor, echo=echo)

    def flash_log(line: str) -> None:
        print("  " + line, flush=True)
        con.note(line)

    flasher = Flasher(args.port, baud=args.flash_baud, esptool=args.esptool, log=flash_log)
    print(f"DFU32 bench: {len(selected)} check(s): {' '.join(t.id for t in selected)}")
    print(f"image {site.version} ({len(site.bin)} B) for board {args.board}; results in {out}/")
    try:
        site.start()
        con.open()
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        site.stop()
        return 2

    ctx = Ctx(args, out, con, site, flasher)
    finished = False
    try:
        try:
            setup(ctx)
        except Exception as e:  # any setup failure, esptool's included; Ctrl+C isn't an Exception and goes on up
            print(f"\nsetup failed: {e}", file=sys.stderr)
            return 2
        if ctx.host_ip:
            print(f"serving the firmware at {site.url(ctx.host_ip)}")
        finished = run_checks(ctx, selected, checks.HOOKS)
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
    finally:
        con.close()
        site.stop()

    print("\n" + "=" * 72)
    bad = 0
    for r in ctx.results:
        if r.status in ("FAIL", "ERROR", "ABORTED"):
            bad += 1
        print(f"  {r.id:6} {r.status:7} {r.title}")
    print(f"\nreport: {out / 'report.md'}   logs: {out / 'serial.log'}, {out / 'http.log'}")
    if not finished:
        print("(run interrupted: the report covers the checks that ran)")
    return 1 if bad or not finished else 0


if __name__ == "__main__":
    sys.exit(main())
