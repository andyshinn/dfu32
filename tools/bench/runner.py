# SPDX-License-Identifier: MIT
"""Test registry, shared context and reporting for the DFU32 bench runner."""
from __future__ import annotations

import json
import re
import socket
import sys
import threading
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

from .api import ApiClient, ApiUnreachable
from .console import ConsoleError, ConsoleRebooted, DownloadMode, SerialConsole, quote_arg
from .flasher import FlashError, Flasher, Layout, LAYOUTS, layout_for_staging
from .parse import parse_flash, parse_job, parse_run, parse_runs, parse_staged, parse_target, parse_token, parse_wifi
from .site import FirmwareSite, local_ip_for

TERMINAL = {"STAGED", "SUCCEEDED", "FAILED"}


class TestFailed(Exception):
    pass


class TestSkipped(Exception):
    pass


# ---- registry ---------------------------------------------------------------------------


@dataclass
class TestDef:
    id: str
    title: str
    fn: Callable
    needs: tuple
    order: tuple

    @property
    def section(self) -> str:
        return self.id.split(".")[0]


REGISTRY: list[TestDef] = []


def _order(tid: str) -> tuple:
    out = []
    for part in re.findall(r"\d+|[a-z]+", tid):
        out.append((0, int(part)) if part.isdigit() else (1, part))
    return tuple(out)


def check(tid: str, title: str, *, needs: tuple = (), after: Optional[str] = None):
    """Registers a bench check. `after` runs it right after another id (execution order only)."""
    def deco(fn):
        order = _order(after) + ((2, 0),) if after else _order(tid)
        REGISTRY.append(TestDef(tid, title, fn, tuple(needs), order))
        return fn
    return deco


def ordered() -> list[TestDef]:
    return sorted(REGISTRY, key=lambda t: t.order)


# ---- results ----------------------------------------------------------------------------


@dataclass
class Result:
    id: str
    title: str
    status: str = "PASS"
    notes: list = field(default_factory=list)
    seconds: float = 0.0
    data: dict = field(default_factory=dict)


@dataclass
class JobWatch:
    job_id: int
    timeline: list = field(default_factory=list)  # (seconds, state, phase, source)
    silent_s: float = 0.0
    silences: int = 0
    samples: list = field(default_factory=list)
    final: dict = field(default_factory=dict)

    def saw(self, state: str) -> bool:
        return any(s == state for _, s, _, _ in self.timeline)

    def time_of(self, state: str) -> Optional[float]:
        for t, s, _, _ in self.timeline:
            if s == state:
                return t
        return None


class Repeater:
    """Optional: the repeater's own USB console, to send `start ota` without a person.

    On native-USB boards (the T096 among them) the port can vanish right after
    `start ota` or `reboot` and come back later, so it is reopened on demand.
    """

    def __init__(self, port: str, baud: int, log, opener: Optional[Callable] = None) -> None:
        self.port = port
        self.baud = baud
        self.log = log
        self._opener = opener or self._pyserial
        self.ser = None
        self._open(timeout=0)  # fail early on a wrong port

    def _pyserial(self, port: str, baud: int):
        import serial  # type: ignore
        return serial.Serial(port, baud, timeout=0.2)

    def _open(self, timeout: float) -> None:
        end = time.monotonic() + timeout
        while True:
            try:
                self.ser = self._opener(self.port, self.baud)
                return
            except Exception:  # noqa: BLE001 - SerialException, OSError, termios.error while re-enumerating
                if time.monotonic() >= end:
                    raise
                time.sleep(0.5)

    def _drop(self) -> None:
        ser, self.ser = self.ser, None
        if ser is not None:
            try:
                ser.close()
            except Exception:  # noqa: BLE001 - closing a vanished port can raise anything
                pass

    def _send(self, data: bytes) -> None:
        if self.ser is None:
            self._open(timeout=0)
        self.ser.reset_input_buffer()
        self.ser.write(data)
        self.ser.flush()

    def _read(self, wait: float) -> tuple[str, bool]:
        """What comes back within `wait` s, and whether the port dropped meanwhile."""
        end = time.monotonic() + wait
        buf = b""
        while time.monotonic() < end:
            try:
                buf += self.ser.read(256)
            except Exception:  # noqa: BLE001 - some boards drop USB right after `start ota`
                self._drop()
                return buf.decode(errors="replace"), True
        return buf.decode(errors="replace"), False

    def command(self, line: str, wait: float = 3.0, *, expect: str = "", window: float = 60.0,
                should_continue: Optional[Callable[[], bool]] = None) -> str:
        """Sends one line and returns the reply.

        The port is opened fresh for every command: after a flash the repeater reboots and its
        USB port comes back as a new device, and an old handle can swallow a write silently.
        With `expect`, the line is sent again until a reply contains it, for up to `window` s,
        which also covers the repeater still booting. `should_continue` is asked before every
        try; when it says no (DFU32 already has the repeater), nothing more is sent. Raises only
        if the line could never be sent at all.
        """
        data = (line + "\r\n").encode()
        deadline = time.monotonic() + window
        sent = 0
        text = ""
        self._drop()
        while True:
            if should_continue is not None and not should_continue():
                self.log(f"repeater: not sending '{line}' (no longer needed)")
                return text
            try:
                self._send(data)
            except Exception as e:  # noqa: BLE001 - not enumerated yet, or a stale handle
                self._drop()
                if time.monotonic() >= deadline:
                    self.log(f"repeater port unavailable for {window:.0f} s ({type(e).__name__}: {e})")
                    if sent:
                        return text
                    raise
                time.sleep(1.5)
                continue
            sent += 1
            text, dropped = self._read(wait)
            self.log(f"repeater> {line} (try {sent}){' [port dropped]' if dropped else ''}\n{text.strip()}")
            if not expect or expect in text or time.monotonic() >= deadline:
                self._drop()
                return text
            time.sleep(1.0)  # no reply yet (still booting, or the write went nowhere): send it again


# ---- context ----------------------------------------------------------------------------

YELLOW, RED, GREEN, BOLD, DIM, RESET = "\033[33m", "\033[31m", "\033[32m", "\033[1m", "\033[2m", "\033[0m"


def _c(code: str, text: str) -> str:
    return f"{code}{text}{RESET}" if sys.stdout.isatty() else text


class Ctx:
    def __init__(self, args, out_dir: Path, console: SerialConsole, site: FirmwareSite, flasher: Flasher) -> None:
        self.args = args
        self.out = out_dir
        self.con = console
        self.site = site
        self.flasher = flasher
        self.redact = console.redact
        self.http_log = open(out_dir / "http.log", "a", encoding="utf-8")
        self.api: Optional[ApiClient] = None
        self.device_ip: Optional[str] = args.device_ip
        self.hostname = ""
        self.host_ip: Optional[str] = args.host_ip
        self.token: Optional[str] = args.token
        self.layout: Optional[Layout] = LAYOUTS.get(args.layout) if args.layout else None
        self.fw_version = ""
        self.tid = args.target_id
        self.mac = (args.mac or "").upper()
        self.result: Optional[Result] = None
        self.results: list[Result] = []
        self.first_seq: Optional[int] = None
        self.repeater: Optional[Repeater] = None
        self.debug_build = False
        self.ota_pending = False  # `start ota` sent and no flash has consumed it yet
        if args.repeater_port:
            self.repeater = Repeater(args.repeater_port, args.repeater_baud, self.con.note)
        if self.device_ip:
            self._make_api(self.device_ip)

    # ---- output ------------------------------------------------------------------------

    def say(self, msg: str) -> None:
        print(f"    {msg}", flush=True)
        self.con.note(msg)

    def note(self, msg: str) -> None:
        if self.result:
            self.result.notes.append(msg)
        self.say(msg)

    def ok(self, msg: str) -> None:
        self.note("ok: " + msg)

    def warn(self, msg: str) -> None:
        if self.result:
            self.result.notes.append("WARN: " + msg)
            if self.result.status == "PASS":
                self.result.status = "WARN"
        print("    " + _c(YELLOW, "WARN: " + msg), flush=True)
        self.con.note("WARN: " + msg)

    def expect(self, cond, msg: str, why: str = "") -> None:
        if not cond:
            raise TestFailed(f"expected: {msg}" + (f"; got: {why}" if why else ""))
        self.ok(msg)

    def soft(self, cond, msg: str, why: str = "") -> bool:
        if cond:
            self.ok(msg)
        else:
            self.warn(f"expected: {msg}" + (f"; got: {why}" if why else ""))
        return bool(cond)

    def data(self, key: str, value) -> None:
        if self.result:
            self.result.data[key] = value

    def action(self, msg: str) -> None:
        print("\a\n    " + _c(BOLD + YELLOW, ">>> " + msg) + "\n", flush=True)
        self.con.note("ACTION: " + msg)

    def pause(self, msg: str) -> None:
        if self.args.unattended:
            raise TestSkipped("needs a person (--unattended): " + msg)
        if not sys.stdin.isatty():
            raise TestSkipped("needs a person at the keyboard: " + msg)
        self.action(msg)
        input("    press Enter to continue... ")
        self.con.note("(Enter pressed)")

    def countdown(self, seconds: int, what: str) -> None:
        end = time.monotonic() + seconds
        while (left := end - time.monotonic()) > 0:
            print(f"\r    {what}: {left:4.0f} s ", end="", flush=True)
            time.sleep(min(1.0, left))
        print("\r" + " " * 60 + "\r", end="", flush=True)

    def start_ota(self, *, lora: bool = False, wait_enter: bool = False, hint: str = "",
                  still_needed: Optional[Callable[[], bool]] = None) -> None:
        """Gets `start ota` to the repeater: over its USB console when available, else by asking.

        Retries stop as soon as `still_needed()` says DFU32 has found the repeater (see
        awaiting()). Otherwise a retry that lands after the flash would put the freshly flashed
        repeater straight back into OTA mode. Without it (e.g. before a BLE scan), it just retries.
        """
        self.ota_pending = True
        if lora and self.args.unattended and self.repeater:
            self.note("--unattended: `start ota` sent over the repeater's USB console instead of LoRa, so the "
                      "remote (LoRa) path was not tested")
            lora = False
        if self.repeater and not lora:
            try:
                out = self.repeater.command("start ota", expect="OK",
                                            should_continue=still_needed)
            except Exception as e:  # couldn't even send it: ask the person instead
                self.warn(f"couldn't send `start ota` over the repeater's USB port ({type(e).__name__}: {e})")
            else:
                if "OK" in out:
                    self.say("sent `start ota` over the repeater's USB console (it replied OK)")
                    if wait_enter:
                        time.sleep(2)
                    return
                if still_needed is not None and not still_needed():
                    self.say("DFU32 has already found the repeater; no `start ota` needed")
                    return
                self.warn("the repeater never answered `start ota` with OK over USB (60 s of retries)")
        where = "OVER LoRa from the companion app (remote admin)" if lora else "to the repeater (LoRa or its USB console)"
        msg = f"Send `start ota` {where}." + (f" {hint}" if hint else "")
        if wait_enter:
            self.pause(msg + " Then press Enter once it has replied OK.")
        else:
            self.action(msg + " The script carries on by itself once DFU32 sees the repeater.")

    def awaiting(self, job_id: int) -> Callable[[], bool]:
        """A still_needed() for start_ota: true while API job `job_id` hasn't found the repeater yet."""
        def check() -> bool:
            job = self.console_job()  # the console answers even while WiFi is off
            if job.get("id") != job_id:
                return True  # couldn't tell; keep trying
            return job.get("state") in ("FETCHING", "VERIFYING_STAGED", "PRECHECK", "AWAITING_TARGET")
        return check

    def _target_advertising(self) -> Optional[bool]:
        """A short BLE scan: is the repeater advertising (app in OTA mode, or its bootloader)? None if unknown."""
        try:
            r = self.ensure_token().post("/ble/scan", {"duration_s": 5, "dfu_only": True}, timeout=25)
        except Exception:
            return None
        if r.status != 200:
            return None
        return any(d.get("matches_target") == self.tid for d in (r.get("devices") or []))

    def ensure_repeater_idle(self) -> None:
        """Checks that expect DFU32 *not* to find the repeater need it out of OTA mode."""
        if self.repeater and self.api:
            # Check rather than trust bookkeeping: scan, and reboot the repeater while it advertises.
            for _ in range(3):
                adv = self._target_advertising()
                if adv is None:
                    break
                if not adv:
                    self.ota_pending = False
                    return
                try:
                    self.repeater.command("reboot", wait=1)
                    self.say("the repeater was still advertising for OTA: rebooted it over USB")
                except Exception as e:
                    self.warn(f"repeater USB console failed ({type(e).__name__}: {e})")
                    break
                time.sleep(12)
        if not self.ota_pending:
            return
        rebooted = False
        if self.repeater:
            try:
                self.repeater.command("reboot", wait=1)
                rebooted = True
                self.say("rebooted the repeater over USB so it stops advertising for OTA")
                time.sleep(10)
            except Exception as e:
                self.warn(f"repeater USB console failed ({type(e).__name__}: {e})")
        if not rebooted:
            self.pause("The repeater is still in OTA mode from an earlier `start ota`. Reboot it (`reboot` on its "
                       "console or over LoRa) so it stops advertising, and wait until it is back up.")
        self.ota_pending = False

    # ---- device / network ---------------------------------------------------------------

    def _make_api(self, ip: str) -> None:
        self.device_ip = ip
        self.api = ApiClient(ip, token=self.token, log=self.http_log, redactor=self.redact)
        if not self.host_ip:
            try:
                self.host_ip = local_ip_for(ip)
            except OSError:
                pass

    def wifi_status(self) -> dict:
        return parse_wifi(self.con.cmd("wifi", 10))

    def ensure_wifi(self, wait: float = 30) -> dict:
        st = self.wifi_status()
        end = time.monotonic() + 300
        while st.get("stopped") and time.monotonic() < end:
            # WiFi is off for a transfer: re-joining now would disturb it; wait for the restore.
            time.sleep(3)
            st = self.wifi_status()
        if not st["connected"] and self.args.wifi_ssid and st.get("ssid") != self.args.wifi_ssid:
            st = self.join_wifi()
        end = time.monotonic() + wait
        while not st["connected"] and time.monotonic() < end:
            time.sleep(3)
            st = self.wifi_status()
        if not st["connected"]:
            if self.args.wifi_ssid:
                st = self.join_wifi()
            if not st["connected"]:
                raise TestSkipped("WiFi is not connected; pass --wifi-ssid/--wifi-pass (or fix the network)")
        self.hostname = st["hostname"]
        if self.api is None or (st["ip"] != self.device_ip and not self.args.device_ip):
            self._make_api(self.args.device_ip or st["ip"])
        return st

    def join_wifi(self) -> dict:
        ssid, pw = self.args.wifi_ssid, self.args.wifi_pass or ""
        if not ssid:
            raise TestSkipped("no --wifi-ssid given")
        self.redact.add(quote_arg(pw))  # the echo shows the escaped form
        out = self.con.cmd(f"wifi {quote_arg(ssid)} {quote_arg(pw) if pw else ''}".rstrip(), 45)
        return parse_wifi(out)

    def ensure_api(self) -> ApiClient:
        self.ensure_wifi()
        if not self.host_ip:
            raise TestFailed("cannot work out this computer's LAN address; pass --host-ip")
        return self.api  # type: ignore[return-value]

    def ensure_token(self) -> ApiClient:
        api = self.ensure_api()
        if self.token:
            r = api.get("/targets")
            if r.status == 200:
                return api
        st = api.status()
        if st and not st["device"]["claimed"]:
            r = api.post("/claim", token=None)
            if r.status == 200 and r.get("token"):
                self.set_token(r.get("token"))
                self.say("claimed the device (new API token)")
                return api
        tok = parse_token(self.con.cmd("token rotate", 10))
        if not tok:
            raise TestFailed("could not get an API token from `token rotate`")
        self.set_token(tok)
        self.say("got a fresh API token with `token rotate` on the console")
        return api

    def set_token(self, tok: str) -> None:
        self.redact.add(tok)
        self.token = tok
        if self.api:
            self.api.token = tok
        (self.out / "token.txt").write_text(tok + "\n")  # handy for curl afterwards; not in any log

    def manifest_url(self, variant: str = "good") -> str:
        if not self.host_ip:
            self.ensure_wifi()
        if not self.host_ip:
            raise TestFailed("cannot work out this computer's LAN address; pass --host-ip")
        return self.site.url(self.host_ip, variant)

    def need_mac(self) -> str:
        if not self.mac:
            t = parse_target(self.con.cmd("target", 10))
            self.mac = t.get("mac", "")
        if not self.mac:
            raise TestSkipped("no repeater MAC: pass --mac XX:XX:XX:XX:XX:XX (from `start ota`'s reply)")
        return self.mac

    def ensure_target_console(self, *, url: bool = True) -> None:
        """Target set from the console (sections 1-2; also what the API sees)."""
        mac = self.need_mac()
        cur = parse_target(self.con.cmd("target", 10))
        if cur.get("id") != self.tid:
            self.con.cmd(f"target id {self.tid}", 10)
        if cur.get("mac") != mac:
            self.con.cmd(f"target {mac} {self.args.addr_type}", 10)
        if cur.get("board") != self.args.board:
            self.con.cmd(f"target board {self.args.board}", 10)
        if url:
            want = self.manifest_url()
            if cur.get("manifest") != want:
                self.con.cmd(f"target url {want}", 10)

    def target_body(self, **over) -> dict:
        body = {"name": f"Bench {self.tid}", "board": self.args.board, "mac": self.need_mac(),
                "addr_type": 1 if self.args.addr_type == "random" else 0, "manifest_url": self.manifest_url()}
        body.update(over)
        return body

    def ensure_target(self, **over) -> dict:
        api = self.ensure_token()
        body = self.target_body(**over)
        r = api.put(f"/targets/{self.tid}", body)
        if r.status == 409 and r.error == "TARGET_LIMIT":
            for t in (api.get("/targets").get("targets") or []):
                if t.get("id") and t["id"] != self.tid:
                    self.say(f"deleting the other target '{t['id']}' (v1 allows one)")
                    api.delete(f"/targets/{t['id']}")
            r = api.put(f"/targets/{self.tid}", body)
        if r.status not in (200, 201):
            raise TestFailed(f"PUT /targets/{self.tid} failed: {r.short()}")
        return r.body

    def staged_matches(self) -> bool:
        st = parse_staged(self.con.cmd("staged", 60))
        return st["verified"] and st["sha256"] == self.site.sha256 and st["board"] == self.args.board

    def ensure_staged(self) -> None:
        """The image under test staged and verified, over WiFi if possible, else with esptool."""
        if self.staged_matches():
            return
        try:
            self.ensure_wifi(wait=10)
            have_wifi = True
        except TestSkipped:
            have_wifi = False
        if have_wifi:
            self.ensure_target_console()
            out = self.con.cmd("stage --force", 180)
            if "STAGE OK" not in out:
                raise TestFailed(f"could not stage the image: {out.strip().splitlines()[-1:]}")
            self.say("staged the image under test over WiFi")
            return
        lay = self.need_layout()
        path = self.out / "staging.img"
        path.write_bytes(self.site.staging_image(self.tid))
        self.say(f"no WiFi: writing the staging image with esptool at 0x{lay.staging:X}")
        self.flash_images([(lay.staging, path)])
        if not self.staged_matches():
            raise TestFailed("the staging image written with esptool doesn't verify")

    def need_layout(self) -> Layout:
        if not self.layout:
            raise TestSkipped("flash layout unknown: pass --layout 4mb or 8mb")
        return self.layout

    # ---- reboot / flashing DFU32 itself -----------------------------------------------------

    def reset_and_wait(self, *, manual: bool = False, why: str = "") -> dict:
        manual = manual or self.args.manual_reset
        for attempt in (1, 2):
            before, mark = len(self.con.boots), self.con.mark()
            if manual:
                self.action(f"Press RESET on the DFU32 board now{(' (' + why + ')') if why else ''}.")
            else:
                self.con.hard_reset()
            try:
                boot = self.con.wait_boot(before, timeout=120 if manual else 45, since=mark)
                break
            except DownloadMode:
                if attempt == 2 or manual:
                    raise ConsoleError("DFU32 keeps resetting into its ROM download mode ('waiting for download'): "
                                       "something holds GPIO0 low. Press RESET on the board (with nothing else "
                                       "holding the port), then run again with --manual-reset if it recurs.")
                self.say("DFU32 came up in ROM download mode; resetting again with DTR released")
        self.after_boot(boot)
        return boot

    def after_boot(self, boot: dict) -> None:
        self.fw_version = boot["version"]
        text = self.con.boot_lines(boot)
        m = re.search(r"partition staging\s+@ 0x([0-9a-fA-F]+)", text)
        if m and not self.args.layout:
            self.layout = layout_for_staging(int(m.group(1), 16)) or self.layout
        m = re.search(r"WiFi connected, IP (\d+\.\d+\.\d+\.\d+)", text)
        if m and m.group(1) != self.device_ip and not self.args.device_ip:
            self._make_api(m.group(1))

    def flash_images(self, items: list, *, erase_all: bool = False) -> dict:
        self.con.note("esptool: " + ", ".join(f"0x{o:X} {p}" for o, p in items))
        self.con.release()
        try:
            self.flasher.write(items, erase_all=erase_all)
        finally:
            self.con.reacquire()
        # esptool reset the chip while the port was closed: reset again so the boot is seen.
        return self.reset_and_wait()

    # ---- console flash -----------------------------------------------------------------

    def console_flash(self, extra: str = "", *, wait: Optional[int] = None, start_ota: bool = True,
                      lora: bool = False, hint: str = "", until: Optional[Callable[[int], bool]] = None) -> dict:
        wait = wait or self.args.start_ota_timeout
        mark = self.con.mark()
        line = f"flash --yes --wait {wait} {extra}".strip()
        start = self.con.send(line)
        hit = self.con.wait_line(r"Waiting up to \d+ s for|failed checks|No target MAC|Busy:", 60, since=mark)
        if hit and "Waiting up to" in hit[1].text:
            if start_ota:
                self.start_ota(lora=lora, hint=hint,
                               still_needed=lambda: self.con.find_line(r"dfu_ble: found ", since=mark) is None)
            else:
                self.say("no `start ota` needed: the repeater's bootloader is waiting for DFU (usually at MAC+1)")
            if until and until(mark):
                return {"interrupted": True}
        out = self.con.finish(start, wait + 900, line)
        res = parse_flash(out)
        res["output"] = out
        res["mark"] = mark
        if res["ok"] or res["attempts"]:
            self.ota_pending = False
        if res["ok"]:
            self.say(f"FLASH OK (run #{res['run']}, {res['throughput_kBps']} kB/s)")
        else:
            self.say(f"flash did not succeed: {res['error']}")
        return res

    def recover_target(self) -> None:
        """Flash the staged image again after an erase; the OTAFIX bootloader waits at MAC+1."""
        self.say("recovering the repeater: flashing the staged image again")
        res = self.console_flash(start_ota=False, wait=180)
        if not res.get("ok"):
            self.action("Recovery flash failed. The repeater may be stuck in its bootloader: run `flash --yes` on "
                        "the DFU32 console, or recover it over USB (docs/BENCH.md, 'If a flash fails').")
            raise TestFailed("could not recover the repeater after the test")

    def run_record(self, seq: int) -> tuple[Optional[dict], int]:
        return parse_run(self.con.cmd(f"run {seq}", 20))

    def runs(self) -> list[dict]:
        return parse_runs(self.con.cmd("runs 50", 20))

    # ---- API jobs ----------------------------------------------------------------------

    def console_job(self) -> dict:
        try:
            return parse_job(self.con.cmd("job", 10))
        except ConsoleRebooted:
            raise
        except ConsoleError:
            return {}

    def api_flash(self, body: Optional[dict] = None, *, expect_status: int = 202):
        api = self.ensure_token()
        body = dict(body or {})
        body.setdefault("arm_timeout_s", self.args.start_ota_timeout)
        before = ((api.status() or {}).get("job") or {}).get("id", 0)
        try:
            r = api.post(f"/targets/{self.tid}/flash", body)
        except ApiUnreachable as e:
            # The reply can get lost even though the job started (WiFi may already be off for the
            # transfer): ask the console, which always answers, before failing.
            job = self.console_job()
            if job.get("kind") == "flash" and job.get("id", 0) > before:
                self.warn(f"POST /flash got no reply ({e}), but job #{job['id']} did start; following it")
                return job["id"], None
            raise
        if expect_status:
            self.expect(r.status == expect_status, f"POST /targets/{self.tid}/flash {json.dumps(body)} -> {expect_status}",
                        r.short())
        return r.get("job"), r

    def api_stage(self, body: Optional[dict] = None):
        api = self.ensure_token()
        r = api.post(f"/targets/{self.tid}/stage", body or {})
        self.expect(r.status == 202, f"POST /targets/{self.tid}/stage {json.dumps(body or {})} -> 202", r.short())
        return r.get("job")

    def wait_job(self, job_id: int, timeout: float, *, on_state: Optional[Callable[[dict], None]] = None,
                 on_sample: Optional[Callable[[dict, str], None]] = None, poll: float = 1.0) -> JobWatch:
        """Follows a job on /status; while the API is silent (WiFi off) it follows `job` on the console."""
        w = JobWatch(job_id)
        t0 = time.monotonic()
        deadline = t0 + timeout
        last = None
        silent_since: Optional[float] = None
        while time.monotonic() < deadline:
            st = self.api.status(timeout=2.5) if self.api else None
            now = time.monotonic()
            if st is None:
                if silent_since is None:
                    silent_since = now
                    self.say("API silent (WiFi off for the transfer?); following the job on the serial console")
                job, source = self.console_job(), "console"
            else:
                if silent_since is not None:
                    w.silent_s += now - silent_since
                    w.silences += 1
                    self.say(f"API answering again after {now - silent_since:.0f} s")
                    silent_since = None
                job, source = st.get("job") or {}, "api"
            if job.get("id") == job_id:
                key = (job.get("state"), job.get("phase", ""))
                if key != last:
                    last = key
                    w.timeline.append((round(now - t0, 1), key[0], key[1], source))
                    self.say(f"[{now - t0:6.1f} s] {key[0]}{' / ' + key[1] if key[1] else ''}"
                             f"{'  (from console)' if source == 'console' else ''}")
                    if on_state:
                        on_state(job)
                if on_sample:
                    on_sample(job, source)
                if job.get("state") in TERMINAL:
                    if job.get("state") == "SUCCEEDED" or job.get("target_erased"):
                        self.ota_pending = False
                    if silent_since is not None:
                        w.silent_s += now - silent_since
                        w.silences += 1
                    w.final = job
                    if job.get("error"):
                        self.say(f"job error: {job['error']}")
                    return w
            time.sleep(poll if source == "api" else 2)
        raise TestFailed(f"job #{job_id} did not finish within {timeout:.0f} s (last state {last})")

    def wait_api(self, timeout: float = 120) -> dict:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            st = self.api.status(timeout=3) if self.api else None
            if st:
                return st
            time.sleep(2)
        raise TestFailed(f"the API did not answer within {timeout:.0f} s")

    def abort_leftover_job(self) -> None:
        st = self.api.status(timeout=3) if self.api else None
        if not st:
            # API silent: a job may be mid-transfer with WiFi off. Don't start the next check on
            # top of it; follow it on the console until it ends (or DFU32 reboots).
            job = self.console_job()
            if job.get("id") and job.get("state") not in TERMINAL and job.get("state") not in ("IDLE", ""):
                self.say(f"job #{job['id']} is still running ({job['state']}); waiting for it to finish")
                end = time.monotonic() + 600
                while time.monotonic() < end:
                    time.sleep(3)
                    try:
                        job = self.console_job()
                    except ConsoleRebooted:
                        self.con.wait_prompt_after_boot()
                        return
                    if job.get("state") in TERMINAL or not job.get("id"):
                        return
            return
        job = st.get("job") or {}
        if job.get("state") not in TERMINAL and job.get("state") not in ("IDLE", None):
            self.say(f"aborting leftover job #{job.get('id')} ({job.get('state')})")
            try:
                self.ensure_token().post("/jobs/current/abort")
                self.wait_job(job["id"], 90)
            except Exception as e:  # best effort
                self.say(f"could not abort it cleanly: {e}")

    def api_flash_and_wait(self, body: dict, *, lora: bool = False, hint: str = "", start_ota: bool = True,
                           on_sample: Optional[Callable[[dict, str], None]] = None, poll: float = 1.0) -> JobWatch:
        """POST /flash, ask for `start ota` once armed, follow the job to its end."""
        job, _ = self.api_flash(body)
        prompted = {"done": False, "next": 0.0}

        def on_state(j: dict) -> None:
            if j.get("state") == "AWAITING_TARGET" and not prompted["done"]:
                prompted["done"] = True
                prompted["next"] = time.monotonic() + 60
                if start_ota:
                    self.start_ota(lora=lora, hint=hint, still_needed=self.awaiting(job))
                else:
                    self.say("no `start ota` needed: the repeater's bootloader is waiting for DFU (usually at MAC+1)")

        def sample(j: dict, src: str) -> None:
            # While DFU32 still waits for the repeater, remind once a minute how long is left.
            if j.get("state") == "AWAITING_TARGET" and prompted["done"] and time.monotonic() >= prompted["next"]:
                prompted["next"] = time.monotonic() + 60
                left = j.get("arm_remaining_s")
                self.say(f"still waiting for the repeater{f' ({left} s left)' if left is not None else ''}: "
                         f"send `start ota`{' over LoRa' if lora else ''} if you haven't")
            if on_sample:
                on_sample(j, src)

        return self.wait_job(job, body.get("arm_timeout_s", self.args.start_ota_timeout) + 900, on_state=on_state,
                             on_sample=sample, poll=poll)

    def recover_target_api(self) -> None:
        self.say("recovering the repeater: flashing the staged image again (source=staged)")
        w = self.api_flash_and_wait({"source": "staged", "wifi_off": False, "arm_timeout_s": 180}, start_ota=False)
        if w.final.get("state") != "SUCCEEDED":
            self.recover_target()

    # ---- mDNS --------------------------------------------------------------------------

    @staticmethod
    def resolve(name: str, timeout: float = 8) -> Optional[str]:
        box: dict = {}

        def run() -> None:
            try:
                box["ip"] = socket.gethostbyname(name)
            except OSError as e:
                box["err"] = str(e)

        th = threading.Thread(target=run, daemon=True)
        th.start()
        th.join(timeout)
        return box.get("ip")


# ---- report -----------------------------------------------------------------------------


def write_report(ctx: Ctx, started: float, finished: bool) -> Path:
    counts: dict = {}
    for r in ctx.results:
        counts[r.status] = counts.get(r.status, 0) + 1
    meta = {"started": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(started)),
            "duration_s": round(time.time() - started), "finished": finished, "dfu32_version": ctx.fw_version,
            "layout": ctx.layout.name if ctx.layout else "", "image_version": ctx.site.version,
            "image_sha256": ctx.site.sha256, "board": ctx.args.board, "target_id": ctx.tid,
            "debug_build_used": ctx.debug_build, "counts": counts}
    data = {"meta": meta, "results": [r.__dict__ for r in ctx.results]}
    (ctx.out / "report.json").write_text(ctx.redact(json.dumps(data, indent=2, default=str)) + "\n")

    md = [f"# DFU32 bench report ({meta['started']})", "",
          f"- DFU32 firmware: `{meta['dfu32_version']}` ({meta['layout'] or 'layout unknown'})",
          f"- Image: `{meta['image_version']}` for `{meta['board']}`, sha256 `{meta['image_sha256'][:16]}…`",
          "- Result: " + ", ".join(f"{v} {k}" for k, v in sorted(counts.items())) +
          ("" if finished else " (run interrupted)"), "",
          "| Check | Result | Time | Notes |", "|---|---|---|---|"]
    for r in ctx.results:
        key = [n for n in r.notes if n.startswith(("FAIL", "WARN", "ERROR", "SKIP"))]
        summary = "; ".join(key)[:300] or (r.notes[-1][:120] if r.notes else "")
        md.append(f"| {r.id} {r.title} | **{r.status}** | {r.seconds:.0f} s | {summary.replace('|', '/')} |")
    md.append("")
    for r in ctx.results:
        md.append(f"## {r.id} {r.title}: {r.status}")
        md.append("")
        md += [f"- {n}" for n in r.notes]
        if r.data.get("table"):
            md += ["", *r.data["table"]]
        md.append("")
    md.append("Logs: `serial.log` (everything DFU32 printed), `http.log` (every API call), `report.json`.")
    path = ctx.out / "report.md"
    path.write_text(ctx.redact("\n".join(md)) + "\n")
    return path


def run_checks(ctx: Ctx, selected: list[TestDef], hooks: dict) -> bool:
    """Runs the checks in order. Returns False if interrupted."""
    started = time.time()
    current_section = None
    interrupted = False
    try:
        for i, td in enumerate(selected):
            if td.section != current_section and current_section != "skipped:" + td.section:
                if current_section and current_section in hooks.get("leave", {}):
                    try:
                        hooks["leave"][current_section](ctx)
                    except (ConsoleError, FlashError, ApiUnreachable, TestFailed) as e:
                        print(f"  section teardown failed: {e}", flush=True)
                current_section = td.section
                if current_section in hooks.get("enter", {}):
                    try:
                        skip = hooks["enter"][current_section](ctx)
                    except (ConsoleError, FlashError, ApiUnreachable, TestFailed, TestSkipped) as e:
                        skip = f"section setup failed: {e}"
                    if skip:
                        for t in [t for t in selected[i:] if t.section == current_section]:
                            ctx.results.append(Result(t.id, t.title, "SKIP", [skip]))
                        current_section = "skipped:" + current_section
                        continue
            if current_section.startswith("skipped:"):
                continue
            res = Result(td.id, td.title)
            ctx.result = res
            ctx.results.append(res)
            print("\n" + _c(BOLD, f"[{td.id}] {td.title}"), flush=True)
            ctx.con.note(f"===== {td.id} {td.title} =====")
            t0 = time.monotonic()
            line0 = ctx.con.mark()
            try:
                td.fn(ctx)
            except TestSkipped as e:
                res.status = "SKIP"
                res.notes.append(f"SKIP: {e}")
            except TestFailed as e:
                res.status = "FAIL"
                res.notes.append(f"FAIL: {e}")
            except (ConsoleError, ApiUnreachable, FlashError) as e:
                res.status = "ERROR"
                res.notes.append(f"ERROR: {type(e).__name__}: {e}")
                if isinstance(e, ConsoleRebooted):
                    time.sleep(2)  # let the new boot print its reset reason
                    res.notes += crash_excerpt(ctx.con, line0)
            except KeyboardInterrupt:
                res.status = "ABORTED"
                res.notes.append("interrupted with Ctrl+C")
                interrupted = True
            except Exception as e:  # a bug in the runner: keep going, keep the traceback
                res.status = "ERROR"
                res.notes.append(f"ERROR: {type(e).__name__}: {e}")
                res.notes.append(traceback.format_exc())
            res.seconds = time.monotonic() - t0
            color = {"PASS": GREEN, "WARN": YELLOW, "SKIP": DIM}.get(res.status, RED)
            tb = res.notes[-1] if res.notes and res.notes[-1].startswith("Traceback") else ""
            msgs = [n for n in res.notes if n.startswith(("FAIL:", "ERROR:", "SKIP:"))]
            last = msgs[-1] if msgs and res.status in ("FAIL", "ERROR", "SKIP") else ""
            print("  => " + _c(color, res.status) + (f"  {last}" if last else ""), flush=True)
            if tb:  # a bug in the runner: show where, so it can be reported
                print(_c(DIM, "\n".join("     " + ln for ln in tb.rstrip().splitlines())), flush=True)
            cleanup(ctx, interrupted)
            write_report(ctx, started, False)
            if interrupted:
                break
        if current_section and current_section in hooks.get("leave", {}) and not interrupted:
            try:
                hooks["leave"][current_section](ctx)
            except (ConsoleError, FlashError, ApiUnreachable, TestFailed) as e:
                print(f"  section teardown failed: {e}", flush=True)
    except KeyboardInterrupt:
        interrupted = True
    write_report(ctx, started, not interrupted)
    return not interrupted


_CRASH = re.compile(r"Guru Meditation|panic|abort\(\)|assert|Backtrace|stack overflow|overflow|Rebooting|"
                    r"rst:0x|interrupted by a reboot|Brownout|watchdog|no sign of life|E \(\d+\)")


def crash_excerpt(con: SerialConsole, since: int) -> list:
    """The lines around an unexpected reboot: what DFU32 printed just before it, and its reset reason."""
    boots = [b["index"] for b in con.boots if b["index"] >= since]
    if not boots:
        return []
    boot = boots[-1]
    lines = con.lines
    before = [ln.text for ln in lines[max(since, boot - 60):boot] if _CRASH.search(ln.text)]
    after = [ln.text for ln in lines[boot:boot + 40] if "interrupted by a reboot" in ln.text]
    out = ["DFU32 rebooted unexpectedly. Last log lines that look relevant (full log in serial.log):"]
    out += [f"    {t}" for t in (before[-25:] + after)] or ["    (nothing obvious; see serial.log)"]
    return out


def cleanup(ctx: Ctx, interrupted: bool) -> None:
    """Leaves the device idle between checks: no job running, console at its prompt."""
    try:
        ctx.con.sync(attempts=2)
    except ConsoleError:
        # A console command (flash, scan) is still running: Ctrl+C stops it at a safe point.
        try:
            ctx.say("console busy: sending Ctrl+C")
            # Ctrl+C must arrive on its own (DFU32 ignores it mixed with other keys), so send
            # nothing else until the command has stopped and printed its prompt.
            if not ctx.con.interrupt(timeout=90):
                ctx.say("no prompt yet after Ctrl+C; still trying")
            ctx.con.sync(attempts=6, timeout=5)
        except ConsoleError as e:
            ctx.say(f"console still not at its prompt: {e}")
    try:
        ctx.abort_leftover_job()
    except Exception:
        pass
