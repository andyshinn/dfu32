# SPDX-License-Identifier: MIT
"""A rough DFU32 simulator for testing the bench runner without hardware.

It speaks the console (as a fake serial port) and the HTTP API closely enough
for the runner's own logic to be exercised: prompts, command output, boot
banners, run records, the job state machine, WiFi going quiet during a
transfer, resets. It downloads manifests from the runner's FirmwareSite for
real, so the broken variants are checked end to end. Timings are compressed.
It is not a model of the firmware: when the real device and this disagree,
the device wins and this file gets fixed.
"""
from __future__ import annotations

import hashlib
import json
import queue
import secrets
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional

FLASH_S = 0.8  # simulated transfer time (x3 with WiFi on, so /status can sample progress)


class FakeDfu32:
    def __init__(self, *, mac: str = "F4:1C:36:A7:33:95", board_profile: str = "feather_esp32_v2",
                 max_targets: int = 5) -> None:
        self.lock = threading.RLock()
        self.out: "queue.Queue[bytes]" = queue.Queue()
        self.inbuf = b""
        self.board_profile = board_profile
        self.mac = mac  # the repeater's MAC: what it advertises in OTA mode
        # Stored targets by id, in slot order (insertion order here), and the default's id.
        self.targets: dict[str, dict] = {}
        self.default_id: Optional[str] = None
        self.max_targets = max_targets
        self.staged: Optional[dict] = None
        self.runs: list[dict] = []
        self.next_seq = 1
        self.claimed = False
        self.token_hash = ""
        self.fail_count = 0
        self.penalty_until = 0.0
        self.next_slot = 0.0
        self.hostname = "dfu32-ab12"
        self.pending_hostname: Optional[str] = None
        self.wifi_ssid = "BenchNet"
        self.wifi_up = True
        self.wifi_stopped = False
        self.ap_on = True
        self.boot_t = time.monotonic()
        self.job: dict = {"id": 0, "state": "IDLE"}
        self.job_abort = threading.Event()
        self.lease: Optional[str] = None
        self.ota_mode = threading.Event()
        self.in_bootloader = False
        self.faults: dict = {}
        self.console_busy: Optional[threading.Thread] = None
        self.console_abort = threading.Event()
        self.logs: list[str] = []
        self.http_port = 0
        self._server: Optional[ThreadingHTTPServer] = None
        self._reset_flag = False
        self._job_marker: Optional[dict] = None
        self.rts = False
        self.dtr = False
        self.timeout = 0.1
        self.closed = False
        self.gen = 0

    # ---- fake serial port -----------------------------------------------------------------

    def opener(self, port: str, baud: int) -> "FakeDfu32":
        self.closed = False
        return self

    def read(self, n: int) -> bytes:
        try:
            return self.out.get(timeout=0.05)
        except queue.Empty:
            return b""

    def write(self, data: bytes) -> int:
        if data == b"\x03":
            self.console_abort.set()
            return 1
        self.inbuf += data
        while b"\r" in self.inbuf:
            line, _, self.inbuf = self.inbuf.partition(b"\r")
            self.inbuf = self.inbuf.lstrip(b"\n")
            self._on_line(line.decode(errors="replace"))
        return len(data)

    def flush(self) -> None:
        pass

    def close(self) -> None:
        self.closed = True

    def __setattr__(self, name, value):
        if name == "rts" and value is False and getattr(self, "rts", False) is True:
            object.__setattr__(self, name, value)
            threading.Thread(target=self.reboot, args=("hard reset",), daemon=True).start()
            return
        object.__setattr__(self, name, value)

    # ---- targets (SPEC.md 9.1: up to max_targets, one of them the default) --------------------

    @staticmethod
    def blank_target(tid: str) -> dict:
        return {"id": tid, "name": "", "mac": "", "addr_type": 1, "board": "", "manifest_url": "", "max_app_size": 0,
                "last_ok_version": "", "last_ok_at": 0}

    @property
    def target(self) -> dict:
        """The default target (what the console acts on); an unsaved blank one when there is none."""
        t = self.targets.get(self.default_id or "")
        return t if t is not None else self.blank_target("")

    def full_reason(self) -> str:
        n = self.max_targets
        return f"this DFU32 holds at most {n} target{'' if n == 1 else 's'}: delete one first"

    def add_target(self, rec: dict, *, make_default: bool = False) -> Optional[str]:
        """Stores a new target; returns the refusal (full) or None. The first target is the default."""
        if len(self.targets) >= self.max_targets:
            return self.full_reason()
        self.targets[rec["id"]] = rec
        if make_default or self.default_id not in self.targets:
            self.default_id = rec["id"]
        return None

    def remove_target(self, tid: str) -> bool:
        """Deletes a target; deleting the default makes the first remaining target the default."""
        if self.targets.pop(tid, None) is None:
            return False
        if self.default_id == tid:
            self.default_id = next(iter(self.targets), None)
        return True

    def rename_target(self, old: str, new: str) -> str:
        if new != old and new in self.targets:
            return f"a target with id '{new}' already exists"
        self.targets = {(new if k == old else k): v for k, v in self.targets.items()}
        self.targets[new]["id"] = new
        if self.default_id == old:
            self.default_id = new
        return ""

    def target_view(self, t: dict) -> dict:
        return {**t, "default": t["id"] == self.default_id}

    @staticmethod
    def valid_id(tid: str) -> bool:
        return 0 < len(tid) <= 31 and all(c.islower() or c.isdigit() or c == "-" for c in tid) and tid.isascii()

    @staticmethod
    def mac_matches(addr: str, mac: str) -> bool:
        """addr is the target's MAC or MAC+1 (its bootloader), as matches_target compares them."""
        if len(addr) != 17 or len(mac) != 17:
            return False
        a, m = addr.upper(), mac.upper()
        return a == m or a == f"{m[:15]}{(int(m[15:], 16) + 1) & 0xFF:02X}"  # the last byte wraps, as on the device

    def emit(self, text: str) -> None:
        self.out.put(text.encode())

    def log(self, tag: str, msg: str, level: str = "I") -> None:
        line = f"{level} ({int((time.monotonic() - self.boot_t) * 1000)}) {tag}: {msg}"
        self.logs.append(line)
        self.emit(f"\x1b[0;32m{line}\x1b[0m\n")

    def prompt(self) -> None:
        self.emit("dfu32> ")

    # ---- boot -----------------------------------------------------------------------------

    def boot(self, first: bool = False) -> None:
        self.gen += 1
        self.boot_t = time.monotonic()
        self.console_abort.set()
        self.job_abort.set()
        self.lease = None
        if self.pending_hostname is not None:
            self.hostname = self.pending_hostname
            self.pending_hostname = None
        self.emit("ets Jun  8 2016 00:22:57\nrst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)\n")
        self.log("dfu32", f"DFU32 v0.3.0-sim (ESP-IDF v6.1), board profile '{self.board_profile}'")
        if self._job_marker:
            m, self._job_marker = self._job_marker, None
            erased = m["transferring"]
            self.log("dfu32", f"previous flash of {m['target']} (\"{m['version']}\") was interrupted by a reboot "
                     f"(SW_RESET){'; the target app may be erased' if erased else ''}", "W")
            self.add_run({"result": "ABORTED_BY_REBOOT", "error": "DFU32 rebooted during the run",
                          "target": m["target"], "image_version": m["version"],
                          "target_erased": "possibly" if erased else "no", "attempts": []})
        self.log("dfu32", "partition staging  @ 0x630000 size 1048576")
        self.log("dfu32", "partition runlog   @ 0x01b000 size 65536")
        self.log("dfu32", "layout self-check OK")
        self.wifi_stopped = False
        self.wifi_up = self.ap_on
        if self.wifi_up:
            self.log("net", f"WiFi connected, IP 127.0.0.1, http://{self.hostname}.local/")
        self.job = {"id": 0, "state": "IDLE"}
        self.job_abort = threading.Event()
        self.console_abort = threading.Event()
        self.log("console", "console ready; type 'help'")
        self.emit("\nYour terminal application does not support escape sequences.\n\n")
        self.prompt()

    def reboot(self, why: str) -> None:
        time.sleep(0.05)
        self.emit("\n")
        self.boot()

    def uptime(self) -> int:
        return int(time.monotonic() - self.boot_t)

    # ---- console -------------------------------------------------------------------------

    def _on_line(self, line: str) -> None:
        if self.console_busy and self.console_busy.is_alive():
            return  # the abort watcher discards other keys
        self.emit(line + "\n")
        argv = line.split()
        if not argv:
            self.prompt()
            return
        fn = getattr(self, "c_" + argv[0], None)
        if fn is None:
            self.emit("Unrecognized command\n")
            self.prompt()
            return
        if argv[0] in ("flash", "stage", "check", "scan"):
            self.console_abort = threading.Event()
            self.console_busy = threading.Thread(target=self._run_cmd, args=(fn, argv), daemon=True)
            self.console_busy.start()
        else:
            self._run_cmd(fn, argv)

    def _run_cmd(self, fn, argv) -> None:
        try:
            fn(argv)
        except _Rebooted:
            return
        except _Aborted:
            self.emit("FLASH ABORTED after 1 attempt(s): aborted from the console\nThe target was not erased.\n")
        self.prompt()

    def c_wifi(self, argv) -> None:
        if len(argv) >= 2:
            self.wifi_ssid = argv[1]
            self.emit(f'Connecting to "{argv[1]}"...\n')
            self.wifi_up = self.ap_on
        self.emit(f"  hostname   {self.hostname} (http://{self.hostname}.local/)\n")
        self.emit(f"  network    {self.wifi_ssid}\n")
        if self.wifi_up and not self.wifi_stopped:
            self.emit(f"  connected  yes, IP 127.0.0.1:{self.http_port}, RSSI -52 dBm\n")
        else:
            self.emit("  connected  no, last disconnect reason 201\n")
        self.emit("  reconnects 0\n  clock      synced (SNTP)\n")

    def print_target(self, t: dict) -> None:
        dflt = " (default target)" if t["id"] == self.default_id else ""
        self.emit(f"  id         {t['id']}{dflt}\n")
        typ = "random" if t["addr_type"] else "public"
        self.emit(f"  mac        {t['mac'] or '(not set)'}" +
                  (f" ({typ}), bootloader after reset at x or y" if t["mac"] else "") + "\n")
        self.emit(f"  board      {t['board'] or '(not set: images are not compared with it)'}\n")
        self.emit(f"  manifest   {t['manifest_url'] or '(not set)'}\n")
        self.emit(f"  max app    {str(t['max_app_size']) + ' B' if t['max_app_size'] else 'default'}\n")

    def show_default(self) -> None:
        if self.default_id not in self.targets:
            self.emit("No target yet. Use: target <MAC> [random|public]\n")
            return
        self.print_target(self.targets[self.default_id])
        n = len(self.targets)
        pos = list(self.targets).index(self.default_id) + 1
        self.emit(f"  default    yes ({pos} of {n} target{'' if n == 1 else 's'}; max {self.max_targets})"
                  f"{': `target list` shows all' if n > 1 else ''}\n")

    def c_target(self, argv) -> None:
        f = argv[1] if len(argv) >= 2 else ""
        v = argv[2] if len(argv) >= 3 else ""
        if not f:
            self.show_default()
        elif f == "list":
            if not self.targets:
                self.emit("No targets yet. Use: target <MAC> [random|public], or target add <id> [<MAC> "
                          "[random|public]]\n")
                return
            self.emit(f"  {'ID':<20}  {'MAC':<17}  {'BOARD':<16}  LAST OK\n")
            for t in self.targets.values():
                self.emit(f"{'*' if t['id'] == self.default_id else ' '} {t['id']:<20}  {t['mac'] or '-':<17}  "
                          f"{t['board'] or '-':<16}  {t['last_ok_version'] or '-'}\n")
            self.emit(f"{len(self.targets)} of {self.max_targets} targets. * = default: check, stage, probe, flash "
                      "and blreset use it (`target use <id>`).\n")
        elif f == "use":
            if v not in self.targets:
                self.emit(f"No target '{v}' (`target list` shows the ids).\n")
                return
            self.default_id = v
            self.emit(f"Default target: '{v}'. check, stage, probe, flash and blreset use the default target.\n")
            self.show_default()
        elif f == "add":
            if not self.valid_id(v):
                self.emit("Not saved: id may contain only a-z, 0-9 and '-'\n")
            elif v in self.targets:
                self.emit(f"Not saved: a target with id '{v}' already exists (`target use {v}` makes it the default).\n")
            else:
                rec = self.blank_target(v)
                if len(argv) >= 4:
                    rec.update(mac=argv[3].upper(), addr_type=0 if len(argv) >= 5 and argv[4] == "public" else 1)
                err = self.add_target(rec, make_default=True)
                if err:
                    self.emit(f"Not saved: {err}\n")
                    return
                self.emit(f"Target '{v}' added; it is now the default target.\n")
                self.show_default()
        elif f in ("clear", "delete"):
            tid = v if f == "delete" else (self.default_id or "")
            if f == "clear" and not tid:
                self.emit("No target to clear.\n")
            elif not self.remove_target(tid):
                self.emit(f"No target '{tid}' (`target list` shows the ids).\n")
            elif not self.targets:
                self.emit(f"Target '{tid}' deleted. No targets left.\n")
            else:
                self.emit(f"Target '{tid}' deleted. The default target is now '{self.default_id}'.\n")
        else:
            # Everything else edits the default target; with none, it creates one (id `target`).
            t = dict(self.targets[self.default_id]) if self.default_id in self.targets else self.blank_target("target")
            if f == "id":
                if not self.valid_id(v):
                    self.emit("Not saved: id may contain only a-z, 0-9 and '-'\n")
                    return
                if self.default_id in self.targets:
                    err = self.rename_target(self.default_id, v)
                    if err:
                        self.emit(f"Not saved: {err}\n")
                        return
                    self.show_default()
                    return
                t["id"] = v
            elif f == "board":
                t["board"] = v
            elif f == "url":
                t["manifest_url"] = v
            elif f == "maxapp":
                t["max_app_size"] = int(v, 0)
            elif len(f) == 17 and f.count(":") == 5:
                t.update(mac=f.upper(), addr_type=0 if v == "public" else 1)
            else:
                self.emit(f"Bad MAC or field '{f}'. Use: target [list | use <id> | add <id> ...]\n")
                return
            if t["id"] in self.targets:
                self.targets[t["id"]].update(t)
            else:
                err = self.add_target(t)
                if err:
                    self.emit(f"Not saved: {err}\n")
                    return
            self.show_default()

    def c_check(self, argv) -> None:
        try:
            m, _ = self.fetch_manifest(self.target["manifest_url"])
        except _FetchError as e:
            self.emit(f"CHECK FAILED: {e}\n")
            return
        ok = not self.target["board"] or self.target["board"] in m["boards"]
        self.emit(f"  version    {m['version']}\n")
        self.emit(f"  boards     {', '.join(m['boards'])}  -> target board \"{self.target['board']}\": "
                  f"{'not set, not compared' if not self.target['board'] else 'match' if ok else 'MISMATCH'}\n")
        same = self.staged and self.staged["sha256"] == m["bin"]["sha256"]
        self.emit(f"  staged     \"{self.staged['version']}\": {'same image' if same else 'different image'}\n"
                  if self.staged else "  staged     nothing\n")

    def c_stage(self, argv) -> None:
        if self.lease:
            self.emit(f"Busy: {self.lease} is using BLE/staging. Wait for it (see `job`), or `job abort`.\n")
            return
        err, reused, n = self.stage(self.target["manifest_url"], "--force" in argv)
        if err:
            self.emit(f"STAGE FAILED: {err}\n")
            if err.startswith("HASH_MISMATCH"):
                self.emit("The previous staged image was invalidated; stage again once this is fixed.\n")
        elif reused:
            self.emit(f"STAGE OK: \"{self.staged['version']}\" was already staged and re-verified (12 ms). Use --force "
                      "to download anyway.\n")
        else:
            self.emit(f"STAGE OK: \"{self.staged['version']}\", {n} B in 1.2 s, verified. `flash --yes` when ready.\n")

    def c_staged(self, argv) -> None:
        s = self.staged
        if not s:
            self.emit("Nothing staged (BAD_MAGIC). Use `stage`, or write one with tools/dfu32_stage_image.py.\n")
            return
        self.emit(f"  bin        {s['size']} B  sha256 {s['sha256']}\n")
        self.emit(f"  board      \"{s['board']}\"  version \"{s['version']}\"  target \"{s['target']}\"\nVerifying...\n")
        self.emit("  result     OK\n")

    def c_scan(self, argv) -> None:
        if self.lease:
            self.emit(f"Busy: {self.lease} is using BLE/staging. Wait for it (see `job`), or `job abort`.\n")
            return
        self.emit("Scanning 2 s...\nADDRESS  TYPE  RSSI  DFU  NAME\n0 device(s).\n")

    def c_token(self, argv) -> None:
        if len(argv) > 1 and argv[1] == "rotate":
            tok = self.issue_token()
            self.emit(f"New API token (the old one stops working):\n{tok}\n")
        elif len(argv) > 1 and argv[1] == "clear":
            self.clear_token()
            self.emit("Token cleared: the API is open (no token needed). `token rotate` sets one again.\n")
        else:
            self.emit("API token required.\n" if self.claimed else "API open: no token set.\n")

    def c_runs(self, argv) -> None:
        if not self.runs:
            self.emit("No runs recorded yet.\n")
            return
        self.emit("  RUN  RESULT              TIME  ATT  ERASED  VERSION / ERROR\n")
        for r in reversed(self.runs):
            ms = r.get("duration_ms", 0)
            err = r.get("error", "")
            self.emit(f"{r['seq']:5}  {r['result']:<17}  {ms // 1000:3}.{(ms % 1000) // 100}s  "
                      f"{len(r.get('attempts', [])):3}  {r.get('target_erased', ''):<6}  {r.get('image_version', '')}"
                      f"{' / ' + err if err else ''}\n")

    def c_run(self, argv) -> None:
        seq = int(argv[1])
        r = next((x for x in self.runs if x["seq"] == seq), None)
        if not r:
            self.emit(f"Run {seq} not found (rotated out, or never recorded).\n")
            return
        log = "I (1) dfu_ble: some log\n"
        self.emit(f"run #{seq}\n{json.dumps(r, separators=(',', ':'))}\n--- log ({len(log)} bytes) ---\n{log}")

    def c_job(self, argv) -> None:
        j = self.job
        if not j.get("id"):
            self.emit("No job since boot.\n")
            return
        self.emit(f"  job        #{j['id']} {j['kind']} ({j['source']}) for '{j.get('target', '')}'\n")
        self.emit(f"  state      {j['state']}{' / ' + j['phase'] if j.get('phase') else ''}\n")
        if j.get("error"):
            self.emit(f"  error      {j['error']}\n")
        if j.get("run"):
            self.emit(f"  run        #{j['run']} (`run {j['run']}`)\n")
        self.emit(f"  lease      {self.lease or 'free'}\n")

    def c_flash(self, argv) -> None:
        if self.lease:
            self.emit(f"Busy: {self.lease} is using BLE/staging. Wait for it (see `job`), or `job abort`.\n")
            return
        if not self.target["mac"]:
            self.emit("No target MAC set. Use: target <MAC> [random|public]\n")
            return
        if not self.staged:
            self.emit("Staged image failed checks: nothing staged. Nothing was touched.\n")
            return
        t = self.target
        if self.staged["board"] and t["board"] and self.staged["board"].lower() != t["board"].lower():
            self.emit(f"BOARD_MISMATCH: the staged image is for ID \"{self.staged['board']}\", target "
                      f"'{t['id'] or 'target'}' is \"{t['board']}\". Nothing was touched.\n")
            return
        interval = int(argv[argv.index("--interval") + 1]) if "--interval" in argv else 30
        wifi_on = not ("--wifi" in argv and argv[argv.index("--wifi") + 1] == "off")  # on by default
        self.lease = "console"
        try:
            self.emit(f"Staged image OK ({self.staged['size']} B, \"{self.staged['version']}\"). Waiting up to 300 s "
                      f"for {self.target['mac']}: run `start ota` on the repeater now. Up to 3 attempt(s).\n")
            wifi_off = not wifi_on
            if wifi_off:
                self.emit("WiFi is off until the flash finishes.\n")
                self.wifi_stopped = True
            r = self.do_flash(interval, abort=self.console_abort, trigger="console", wifi_off=wifi_off,
                              t=self.target)
            if wifi_off:
                self.wifi_stopped = False
                self.emit("WiFi restored.\n")
            self.emit("  throughput        12.4 kB/s\n")
            if len(r["attempts"]) > 1:
                for a in r["attempts"]:
                    tail = "" if a["kind"] == "NONE" else f" [{a['kind']}, 27000 B acked]"
                    self.emit(f"  attempt {a['n']}         interval {a['interval_ms']} ms, PRN {a['prn']}: {a['outcome']}{tail}\n")
            self.emit(f"FLASH OK in 38.4 s{' (after retries)' if len(r['attempts']) > 1 else ''}. Check the "
                      "repeater with `ver`.\n")
            self.emit(f"Recorded as run #{r['seq']} (`run {r['seq']}` for details).\n")
        finally:
            self.lease = None
            self.wifi_stopped = False

    # ---- flashing ---------------------------------------------------------------------------

    def start_ota(self) -> None:
        self.ota_mode.set()

    def do_flash(self, interval: int, *, abort, trigger: str, wifi_off: bool, source: str = "staged",
                 on_progress=None, on_seen=None, seconds: float = FLASH_S, t: Optional[dict] = None) -> dict:
        """Waits for `start ota` (or the bootloader), transfers, records the run against target `t`."""
        t = t if t is not None else self.target
        gen = self.gen
        self._job_marker = {"target": t["mac"], "version": self.staged["version"], "transferring": False}

        def check() -> None:
            if self.gen != gen:
                raise _Rebooted()
            if abort.is_set():
                self._job_marker = None
                raise _Aborted()

        while not (self.ota_mode.is_set() or self.in_bootloader):
            check()
            time.sleep(0.02)
        self.ota_mode.clear()
        if on_seen:
            on_seen()
        check()
        attempts = []
        if interval < 11:
            attempts.append({"n": 1, "interval_ms": interval, "prn": 4, "outcome": "link lost",
                             "kind": "LINK_OR_OVERRUN", "bytes_acked": 27000})
            interval = 30
        self._job_marker["transferring"] = True
        self.in_bootloader = True
        steps = 10
        for i in range(steps + 1):
            check()
            pct = i * 10
            self.log("dfu_ble", f"progress {pct:3}% ({pct * 4136}/413680)")
            if on_progress:
                on_progress(i * 41368, 413680)
            time.sleep(seconds / steps)
        self.in_bootloader = False
        attempts.append({"n": len(attempts) + 1, "interval_ms": interval, "prn": 4, "outcome": "OK", "kind": "NONE",
                         "bytes_acked": 413680})
        self._job_marker = None
        rec = self.add_run({"result": "SUCCEEDED", "error": "", "failure_kind": "NONE", "target": t["mac"],
                            "image_version": self.staged["version"], "duration_ms": 38400, "target_erased": "yes",
                            "wifi_off": wifi_off, "target_id": t["id"], "trigger": trigger, "source": source,
                            "started_unix": int(time.time()), "rate_x10_kBps": 124 if wifi_off else 118,
                            "attempts": attempts})
        t["last_ok_version"] = self.staged["version"]
        t["last_ok_at"] = int(time.time())
        return rec

    def add_run(self, rec: dict) -> dict:
        rec = {"seq": self.next_seq, **rec}
        self.next_seq += 1
        self.runs.append(rec)
        return rec

    # ---- staging ----------------------------------------------------------------------------

    def fetch(self, url: str) -> bytes:
        try:
            with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(url, timeout=5) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            raise _FetchError(f"HTTP_ERROR: HTTP {e.code}") from e

    def fetch_manifest(self, url: str):
        raw = self.fetch(url)
        return json.loads(raw), hashlib.sha256(raw).hexdigest()

    def stage(self, url: str, force: bool, t: Optional[dict] = None):
        t = t if t is not None else self.target
        try:
            m, msha = self.fetch_manifest(url)
        except _FetchError as e:
            return str(e), False, 0
        if t["board"] and t["board"] not in m["boards"]:
            return f"BOARD_MISMATCH: manifest is for {m['boards']}, target is \"{t['board']}\"", False, 0
        if not force and self.staged and self.staged["sha256"] == m["bin"]["sha256"]:
            return "", True, 0
        base = url.rsplit("/", 1)[0]
        self.fetch(f"{base}/{m['dat']['url']}")
        data = self.fetch(f"{base}/{m['bin']['url']}")
        if len(data) != m["bin"]["size"]:
            return f"SIZE_MISMATCH: server sends {len(data)} B, manifest says {m['bin']['size']}", False, 0
        if hashlib.sha256(data).hexdigest() != m["bin"]["sha256"]:
            self.staged = None
            return "HASH_MISMATCH: downloaded image doesn't match the manifest", False, 0
        board = t["board"] or (m["boards"][0] if len(m["boards"]) == 1 else "")
        self.staged = {"sha256": m["bin"]["sha256"], "version": m["version"], "board": board,
                       "target": t["id"], "size": len(data), "manifest_sha256": msha}
        return "", False, len(data)

    # ---- API ----------------------------------------------------------------------------------

    def issue_token(self) -> str:
        tok = secrets.token_urlsafe(32)
        self.token_hash = hashlib.sha256(tok.encode()).hexdigest()
        self.claimed = True
        self.reset_throttle()
        return tok

    def clear_token(self) -> None:
        self.claimed = False
        self.token_hash = ""
        self.reset_throttle()

    def reset_throttle(self) -> None:  # a new credential starts with a clean slate, as on the device
        self.fail_count = 0
        self.penalty_until = 0.0
        self.next_slot = 0.0

    def start_http(self) -> int:
        dev = self

        class H(BaseHTTPRequestHandler):
            def _go(self, method: str) -> None:
                if not dev.wifi_up or dev.wifi_stopped:
                    self.close_connection = True
                    self.connection.close()
                    return
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n) if n else b""
                status, obj, hdrs = dev.handle(method, self.path, body, self.headers.get("Authorization", ""))
                raw = json.dumps(obj).encode() if obj is not None else b""
                self.send_response(status)
                for k, v in hdrs.items():
                    self.send_header(k, v)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def do_GET(self):  # noqa: N802
                self._go("GET")

            def do_POST(self):  # noqa: N802
                self._go("POST")

            def do_PUT(self):  # noqa: N802
                self._go("PUT")

            def do_DELETE(self):  # noqa: N802
                self._go("DELETE")

            def log_message(self, *a):
                pass

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self._server.daemon_threads = True
        self.http_port = self._server.server_address[1]
        threading.Thread(target=self._server.serve_forever, daemon=True).start()
        return self.http_port

    def stop(self) -> None:
        if self._server:
            self._server.shutdown()
            self._server.server_close()
            self._server = None

    def auth(self, header: str):
        if not self.claimed:
            return None  # token auth is opt-in: open until a token is set
        now = time.monotonic()
        if now < self.penalty_until:
            if now < self.next_slot:
                return 429, {"error": "RATE_LIMITED"}, {"Retry-After": "1"}
            self.next_slot = now + 1
        tok = header[7:] if header.startswith("Bearer ") else ""
        if tok and hashlib.sha256(tok.encode()).hexdigest() == self.token_hash:
            return None
        self.fail_count += 1
        self.log("api", "rejected request: wrong token", "W")
        if self.fail_count >= 5:
            self.penalty_until = now + 60
        return 401, {"error": "UNAUTHORIZED"}, {}

    def status_obj(self) -> dict:
        # Public, so never the MACs.
        targets = [{"id": t["id"], "name": t["name"], "board": t["board"], "last_ok_version": t["last_ok_version"],
                    "last_ok_at": t["last_ok_at"], "default": t["id"] == self.default_id}
                   for t in self.targets.values()]
        return {"device": {"name": "DFU32", "version": "v0.3.0-sim", "uptime_s": self.uptime(), "claimed": self.claimed,
                           "hostname": self.hostname, "unix_time": int(time.time()),
                           "wifi": {"connected": True, "rssi": -52, "ip": "127.0.0.1"}, "busy": self.lease},
                "job": dict(self.job),
                "targets": targets,
                "default_target": self.default_id if self.targets else None,
                "staged": self.staged}

    @staticmethod
    def no_target(tid: str):
        return 404, {"error": "NOT_FOUND", "detail": f"no target '{tid}'"}, {}

    def put_target(self, tid: str, data: dict):
        """PUT /targets/{id}: a new id is always a new target (409 TARGET_LIMIT when full)."""
        if not self.valid_id(tid):
            return 400, {"error": "BAD_REQUEST", "detail": "id may contain only a-z, 0-9 and '-'"}, {}
        if not isinstance(data, dict) or ("default" in data and not isinstance(data["default"], bool)):
            return 400, {"error": "BAD_REQUEST", "detail": "body must be a JSON object; default a boolean"}, {}
        mac = data.get("mac")
        if mac and not (isinstance(mac, str) and len(mac) == 17 and mac.count(":") == 5):
            return 400, {"error": "BAD_REQUEST", "detail": "MAC must look like AA:BB:CC:DD:EE:FF"}, {}
        existing = self.targets.get(tid)
        rec = dict(existing) if existing else self.blank_target(tid)
        rec.update({k: v for k, v in data.items() if k in rec and k not in ("id", "last_ok_version", "last_ok_at")})
        rec["mac"] = rec["mac"].upper()
        if existing:
            existing.update(rec)
            if data.get("default") is True:
                self.default_id = tid
            return 200, self.target_view(existing), {}
        err = self.add_target(rec, make_default=data.get("default") is True)
        if err:
            return 409, {"error": "TARGET_LIMIT", "detail": err}, {}
        return 201, self.target_view(rec), {}

    def upload_staging(self, query: str, body: bytes):
        """POST /staging: checked against target= when given, else the default or the first target that fits."""
        q = {k: v[0] for k, v in urllib.parse.parse_qs(query).items()}
        board = q.get("board", "")
        ok = lambda t: not board or not t["board"] or board.lower() == t["board"].lower()  # noqa: E731
        against = None
        if q.get("target"):
            against = self.targets.get(q["target"])
            if against is None:
                return self.no_target(q["target"])
            if not ok(against):
                return 422, {"error": "BOARD_MISMATCH", "detail": f"the image is for ID \"{board}\", target "
                                                                  f"'{against['id']}' is \"{against['board']}\""}, {}
        elif self.targets:
            dflt = self.targets.get(self.default_id or "")
            against = dflt if dflt is not None and ok(dflt) else next((t for t in self.targets.values() if ok(t)), None)
            if against is None:
                return 422, {"error": "BOARD_MISMATCH", "detail": f"the image is for ID \"{board}\", and no stored "
                                                                  "target has that ID"}, {}
        if self.lease:
            return 409, {"error": "BUSY"}, {}
        sha = hashlib.sha256(body).hexdigest()
        if q.get("sha256") and q["sha256"] != sha:
            return 422, {"error": "HASH_MISMATCH", "detail": "the body doesn't match sha256"}, {}
        if against is not None and against["max_app_size"] and len(body) > against["max_app_size"]:
            return 413, {"error": "IMAGE_TOO_LARGE", "detail": f"over target '{against['id']}''s max_app_size"}, {}
        self.staged = {"sha256": sha, "version": q.get("version", ""), "board": board, "target": "",
                       "size": len(body), "manifest_sha256": ""}
        self.log("api", f"image upload: \"{self.staged['version']}\", {len(body)} B, checked against "
                        f"{against['id'] if against else 'no target'}")
        return 200, {"target": "", "board": board, "version": self.staged["version"], "bin_size": len(body),
                     "sha256": sha, "manifest_url": "upload:" + q.get("name", "")}, {}

    def handle(self, method: str, path: str, body, authz: str):
        path, _, query = path.partition("?")
        parts = [p for p in path.split("/") if p][2:]  # drop api, v1
        if isinstance(body, str):
            body = body.encode()
        if parts == ["staging"] and method == "POST":
            data: dict = {}
        else:
            data = json.loads(body) if body.strip() else {}
        if parts == ["status"]:
            return 200, self.status_obj(), {}
        if parts == ["claim"]:
            if self.claimed:
                return 409, {"error": "ALREADY_CLAIMED"}, {}
            return 200, {"token": self.issue_token()}, {}
        denied = self.auth(authz)
        if denied:
            return denied
        if parts == ["targets"]:
            return 200, {"targets": [self.target_view(t) for t in self.targets.values()],
                         "default": self.default_id if self.targets else None, "max_targets": self.max_targets}, {}
        if parts[:1] == ["targets"] and len(parts) >= 2:
            tid = parts[1]
            if len(parts) == 2:
                if method == "GET":
                    if tid not in self.targets:
                        return self.no_target(tid)
                    last = next((r for r in reversed(self.runs) if r.get("target_id") == tid), None)
                    return 200, {**self.target_view(self.targets[tid]), "last_run": last}, {}
                if self.lease:
                    return 409, {"error": "BUSY"}, {}
                if method == "DELETE":
                    return (204, None, {}) if self.remove_target(tid) else self.no_target(tid)
                return self.put_target(tid, data)
            sub = parts[2]
            if sub == "default" and method == "POST":
                if self.lease:
                    return 409, {"error": "BUSY"}, {}
                if tid not in self.targets:
                    return self.no_target(tid)
                self.default_id = tid
                return 200, {"default": tid}, {}
            t = self.targets.get(tid)
            if t is None:
                return self.no_target(tid)
            if sub == "check":
                m, _ = self.fetch_manifest(t["manifest_url"])
                same = bool(self.staged and self.staged["sha256"] == m["bin"]["sha256"])
                return 200, {"version": m["version"],
                             "board_ok": not t["board"] or t["board"] in m["boards"],
                             "staged_same": same, "last_ok_version": t["last_ok_version"],
                             "update_available": m["version"] != t["last_ok_version"]}, {}
            if sub == "runs":
                return 200, {"runs": [r for r in reversed(self.runs) if r.get("target_id") == tid]}, {}
            if sub in ("stage", "flash"):
                if self.lease:
                    return 409, {"error": "BUSY"}, {}
                jid = self.job.get("id", 0) + 1
                self.job = {"id": jid, "state": "FETCHING", "kind": sub, "phase": "FETCH",
                            "source": data.get("source", "manifest"), "target": tid, "bytes_done": 0,
                            "bytes_total": 0, "rate_bps": 0, "target_erased": False, "wifi_off": False}
                self.lease = "job"
                self.job_abort = threading.Event()
                threading.Thread(target=self.run_job, args=(sub, data, t), daemon=True).start()
                return 202, {"job": jid, "state": "FETCHING", "poll": "/api/v1/status"}, {}
        if parts == ["jobs", "current", "abort"]:
            if self.job.get("state") in ("IDLE", "STAGED", "SUCCEEDED", "FAILED"):
                return 409, {"error": "NO_JOB"}, {}
            self.job_abort.set()
            erased = bool(self.job.get("target_erased"))
            out = {"aborting": True, "state": self.job["state"], "target_erased": erased}
            if erased:
                out["warning"] = "The transfer had started, so the target's app is erased."
            return 202, out, {}
        if parts[:1] == ["runs"]:
            r = next((x for x in self.runs if x["seq"] == int(parts[1])), None)
            return (200, {"run": r, "log": ""}, {}) if r else (404, {"error": "NOT_FOUND"}, {})
        if parts == ["staging"]:
            if method == "POST":
                return self.upload_staging(query, body)
            if method == "DELETE":
                self.staged = None
                return 204, None, {}
            return 200, {"valid": bool(self.staged)}, {}
        if parts == ["ble", "scan"]:
            if self.lease:
                return 409, {"error": "BUSY"}, {}
            time.sleep(min(2, data.get("duration_s", 8)) / 4)
            devs = []
            if self.ota_mode.is_set():
                # matches_target: the first target (slot order) whose MAC is the address or address-1.
                match = next((t["id"] for t in self.targets.values() if self.mac_matches(self.mac, t["mac"])), None)
                devs.append({"addr": self.mac.upper(), "name": "T096_OTA", "rssi": -60, "dfu": "app_buttonless",
                             "matches_target": match})
            return 200, {"devices": devs}, {}
        if parts == ["logs"]:
            since = int(query.split("=")[1]) if query.startswith("since=") else max(0, len(self.logs) - 20)
            return 200, {"next": len(self.logs), "text": "\n".join(self.logs[since:])}, {}
        if parts == ["config"]:
            if method == "PUT":
                changed = data.get("hostname") is not None and data["hostname"] != self.hostname
                if changed:
                    self.pending_hostname = data["hostname"] or "dfu32-ab12"
                return 200, {"hostname": self.pending_hostname or self.hostname, "reboot_required": changed}, {}
            return 200, {"hostname": "" if self.hostname == "dfu32-ab12" else self.hostname}, {}
        if parts == ["system", "reboot"]:
            threading.Thread(target=self.reboot, args=("api",), daemon=True).start()
            return 202, {"rebooting": True}, {}
        if parts == ["system", "token", "clear"] and method == "POST":
            self.clear_token()
            return 200, {"claimed": False}, {}
        if parts == ["debug", "fault"]:
            self.faults = data
            return 200, data, {}
        return 404, {"error": "NOT_FOUND"}, {}

    def set_job(self, **kw) -> None:
        with self.lock:
            self.job.update(kw)

    def run_job(self, kind: str, data: dict, t: dict) -> None:
        try:
            if data.get("source") != "staged":
                err, reused, _ = self.stage(data.get("url") or t["manifest_url"], data.get("force", False), t)
                if err:
                    self.set_job(state="FAILED", error=err)
                    return
                if kind == "stage":
                    self.set_job(state="STAGED", phase="DONE", reused=reused)
                    return
            else:
                self.set_job(state="VERIFYING_STAGED")
                if not self.staged:
                    self.set_job(state="FAILED", error="NO_STAGED_IMAGE: BAD_MAGIC")
                    return
                if self.staged["board"] and t["board"] and self.staged["board"] != t["board"]:
                    self.set_job(state="FAILED", error=f"BOARD_MISMATCH: staged image is for board "
                                                       f"\"{self.staged['board']}\"")
                    return
            self.set_job(state="AWAITING_TARGET", phase="SCAN", bytes_total=self.staged["size"])
            arm = data.get("arm_timeout_s", 600)
            wifi_off = data.get("wifi_off", False)
            faults, self.faults = self.faults, {}
            timeout = threading.Event()
            deadline = threading.Event()
            seen_box = {"seen": False}
            threading.Timer(arm / 10, lambda: seen_box["seen"] or timeout.set()).start()  # compressed
            abort = _Either(self.job_abort, timeout, deadline)

            def seen():
                seen_box["seen"] = True
                self.set_job(state="RADIO_HANDOVER", phase="HANDOVER")
                if wifi_off:
                    self.wifi_stopped = True
                self.log("job", f"target seen; WiFi {'stopped' if wifi_off else 'kept up'}, run deadline "
                                f"{faults.get('deadline_s') or 600} s")
                if faults.get("deadline_s"):
                    def fire():
                        self.log("job", "run deadline exceeded: aborting the BLE link", "E")
                        deadline.set()
                    threading.Timer(faults["deadline_s"] / 10, fire).start()
                if faults.get("stall_s"):
                    self.log("job", f"fault injection: blocking the DFU task {faults['stall_s']} s without a heartbeat",
                             "W")
                    time.sleep(1.0)
                    self.log("job", "DFU task shows no sign of life for 30 s: rebooting", "E")
                    self.reboot("stall")
                    raise _Rebooted()
                self.set_job(state="DFU_RUNNING", phase="STREAM")

            def prog(done, total):
                self.set_job(bytes_done=done, rate_bps=12400, target_erased=True)
                if faults.get("deadline_s"):
                    time.sleep(0.2)

            try:
                rec = self.do_flash(20, abort=abort, trigger="api", wifi_off=wifi_off, t=t,
                                    source=data.get("source", "manifest"), on_progress=prog, on_seen=seen,
                                    seconds=FLASH_S if wifi_off else 3 * FLASH_S)
            except _Rebooted:
                return
            except _Aborted:
                self.wifi_stopped = False
                erased = self.job.get("target_erased", False)
                src = data.get("source", "manifest")
                if deadline.is_set():
                    rec = self.add_run({"result": "FAILED", "error": "DEADLINE_EXCEEDED: took too long",
                                        "target_id": t["id"], "trigger": "api", "source": src,
                                        "started_unix": int(time.time()), "attempts": []})
                    self.set_job(state="FAILED", error="DEADLINE_EXCEEDED: took too long", run=rec["seq"])
                elif timeout.is_set() and not self.job_abort.is_set():
                    rec = self.add_run({"result": "FAILED", "error": "TARGET_NOT_FOUND", "target_id": t["id"],
                                        "trigger": "api", "source": src, "started_unix": int(time.time()),
                                        "attempts": []})
                    self.set_job(state="FAILED", error="TARGET_NOT_FOUND: no advert", run=rec["seq"])
                else:
                    self.set_job(state="FAILED", error="ABORTED", target_erased=erased)
                return
            self.set_job(state="RESTORING", phase="RESTORE", run=rec["seq"])
            if faults.get("restore_fail") and wifi_off:
                self.log("job", "fault injection: treating WiFi restore as failed", "W")
                time.sleep(0.5)
                self.log("job", "WiFi not connected 60 s after restore; retrying once", "W")
                time.sleep(0.5)
                self.log("job", "WiFi still down after a retry: rebooting so the network comes back (SPEC.md 7.3)", "E")
                self.reboot("restore")
                return
            time.sleep(0.3)
            self.wifi_stopped = False
            self.set_job(state="SUCCEEDED", phase="DONE")
        finally:
            self.lease = None


class _Either:
    def __init__(self, *events: threading.Event) -> None:
        self.events = events

    def is_set(self) -> bool:
        return any(e.is_set() for e in self.events)


class _Aborted(Exception):
    pass


class _Rebooted(Exception):
    pass


class _FetchError(Exception):
    pass
