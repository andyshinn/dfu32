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
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional

FLASH_S = 0.8  # simulated transfer time (x3 with WiFi on, so /status can sample progress)


class FakeDfu32:
    def __init__(self, *, mac: str = "F4:1C:36:A7:33:95", board_profile: str = "feather_esp32_v2") -> None:
        self.lock = threading.RLock()
        self.out: "queue.Queue[bytes]" = queue.Queue()
        self.inbuf = b""
        self.board_profile = board_profile
        self.mac = mac
        self.target = {"id": "", "mac": "", "board": "", "manifest_url": "", "name": "", "last_ok_version": "",
                       "addr_type": 1}
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

    def c_target(self, argv) -> None:
        t = self.target
        if len(argv) >= 2:
            f, v = argv[1], argv[2] if len(argv) > 2 else ""
            if f == "board":
                t["board"] = v
            elif f == "url":
                t["manifest_url"] = v
            elif f == "id":
                t["id"] = v
            elif f == "clear":
                self.target = {k: "" for k in t}
                self.emit("Target cleared.\n")
                return
            else:
                t["mac"] = f.upper()
        self.emit(f"  id         {t['id'] or 'default'}{' (default)' if not t['id'] else ''}\n")
        self.emit(f"  mac        {t['mac'] or '(not set)'}" + (" (random), bootloader after reset at x or y" if t["mac"] else "") + "\n")
        self.emit(f"  board      {t['board'] or '(not set: images are not compared with it)'}\n")
        self.emit(f"  manifest   {t['manifest_url'] or '(not set)'}\n  max app    default\n")

    def c_check(self, argv) -> None:
        try:
            m, _ = self.fetch_manifest(self.target["manifest_url"])
        except _FetchError as e:
            self.emit(f"CHECK FAILED: {e}\n")
            return
        ok = not self.target["board"] or self.target["board"] in m["boards"]
        self.emit(f"  version    {m['version']}\n")
        self.emit(f"  boards     {', '.join(m['boards'])}  -> target board \"{self.target['board']}\": "
                  f"{'match' if ok else 'MISMATCH'}\n")
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
        self.emit(f"  job        #{j['id']} {j['kind']} ({j['source']}) for '{self.target['id']}'\n")
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
        if not self.staged:
            self.emit("Staged image failed checks: nothing staged. Nothing was touched.\n")
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
            r = self.do_flash(interval, abort=self.console_abort, trigger="console", wifi_off=wifi_off)
            if wifi_off:
                self.wifi_stopped = False
                self.emit("WiFi restored.\n")
            self.emit("  throughput        12.4 kB/s\n")
            if len(r["attempts"]) > 1:
                for a in r["attempts"]:
                    tail = "" if a["kind"] == "NONE" else f" [{a['kind']}, 27000 B acked]"
                    self.emit(f"  attempt {a['n']}         interval {a['interval_ms']} ms, PRN 8: {a['outcome']}{tail}\n")
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
                 on_progress=None, on_seen=None, seconds: float = FLASH_S) -> dict:
        """Waits for `start ota` (or the bootloader), transfers, records the run."""
        gen = self.gen
        self._job_marker = {"target": self.target["mac"], "version": self.staged["version"], "transferring": False}

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
            attempts.append({"n": 1, "interval_ms": interval, "prn": 8, "outcome": "link lost",
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
        attempts.append({"n": len(attempts) + 1, "interval_ms": interval, "prn": 8, "outcome": "OK", "kind": "NONE",
                         "bytes_acked": 413680})
        self._job_marker = None
        rec = self.add_run({"result": "SUCCEEDED", "error": "", "failure_kind": "NONE", "target": self.target["mac"],
                            "image_version": self.staged["version"], "duration_ms": 38400, "target_erased": "yes",
                            "wifi_off": wifi_off, "target_id": self.target["id"], "trigger": trigger, "source": source,
                            "started_unix": int(time.time()), "rate_x10_kBps": 124 if wifi_off else 118,
                            "attempts": attempts})
        self.target["last_ok_version"] = self.staged["version"]
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

    def stage(self, url: str, force: bool):
        try:
            m, msha = self.fetch_manifest(url)
        except _FetchError as e:
            return str(e), False, 0
        if self.target["board"] and self.target["board"] not in m["boards"]:
            return f"BOARD_MISMATCH: manifest is for {m['boards']}, target is \"{self.target['board']}\"", False, 0
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
        self.staged = {"sha256": m["bin"]["sha256"], "version": m["version"], "board": self.target["board"],
                       "target": self.target["id"], "size": len(data), "manifest_sha256": msha}
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
                body = self.rfile.read(n).decode() if n else ""
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
        t = self.target
        return {"device": {"name": "DFU32", "version": "v0.3.0-sim", "uptime_s": self.uptime(), "claimed": self.claimed,
                           "hostname": self.hostname, "unix_time": int(time.time()),
                           "wifi": {"connected": True, "rssi": -52, "ip": "127.0.0.1"}, "busy": self.lease},
                "job": dict(self.job),
                "targets": [{"id": t["id"], "board": t["board"], "last_ok_version": t["last_ok_version"]}] if t["id"] else [],
                "staged": self.staged}

    def handle(self, method: str, path: str, body: str, authz: str):
        path, _, query = path.partition("?")
        parts = [p for p in path.split("/") if p][2:]  # drop api, v1
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
            return 200, {"targets": [dict(self.target)] if self.target["id"] else []}, {}
        if parts[:1] == ["targets"] and len(parts) >= 2:
            tid = parts[1]
            if len(parts) == 2:
                if method == "GET":
                    if tid != self.target["id"]:
                        return 404, {"error": "NOT_FOUND"}, {}
                    last = next((r for r in reversed(self.runs) if r.get("target_id") == tid), None)
                    return 200, {**self.target, "last_run": last}, {}
                if self.lease:
                    return 409, {"error": "BUSY"}, {}
                if method == "DELETE":
                    self.target = {k: "" for k in self.target}
                    return 204, None, {}
                if self.target["id"] and self.target["id"] != tid:
                    return 409, {"error": "TARGET_LIMIT"}, {}
                existed = bool(self.target["id"])
                self.target.update({k: v for k, v in data.items() if k in self.target})
                self.target["id"] = tid
                return (200 if existed else 201), dict(self.target), {}
            sub = parts[2]
            if sub == "check":
                m, _ = self.fetch_manifest(self.target["manifest_url"])
                same = bool(self.staged and self.staged["sha256"] == m["bin"]["sha256"])
                return 200, {"version": m["version"],
                             "board_ok": not self.target["board"] or self.target["board"] in m["boards"],
                             "staged_same": same, "last_ok_version": self.target["last_ok_version"],
                             "update_available": m["version"] != self.target["last_ok_version"]}, {}
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
                threading.Thread(target=self.run_job, args=(sub, data), daemon=True).start()
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
                devs.append({"addr": self.target["mac"], "name": "T096_OTA", "rssi": -60, "dfu": "app_buttonless",
                             "matches_target": self.target["id"]})
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

    def run_job(self, kind: str, data: dict) -> None:
        try:
            if data.get("source") != "staged":
                err, reused, _ = self.stage(data.get("url") or self.target["manifest_url"], data.get("force", False))
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
                if self.staged["board"] and self.target["board"] and self.staged["board"] != self.target["board"]:
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
                rec = self.do_flash(20, abort=abort, trigger="api", wifi_off=wifi_off,
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
                                        "target_id": self.target["id"], "trigger": "api", "source": src,
                                        "started_unix": int(time.time()), "attempts": []})
                    self.set_job(state="FAILED", error="DEADLINE_EXCEEDED: took too long", run=rec["seq"])
                elif timeout.is_set() and not self.job_abort.is_set():
                    rec = self.add_run({"result": "FAILED", "error": "TARGET_NOT_FOUND", "target_id": self.target["id"],
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
