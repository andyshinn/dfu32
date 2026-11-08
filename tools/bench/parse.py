# SPDX-License-Identifier: MIT
"""Parsers for DFU32 console output (main/console.cpp). Pure functions, unit-tested."""
from __future__ import annotations

import json
import re
from typing import Optional


def _field(out: str, name: str) -> Optional[str]:
    """Value of an indented `  name   value` line."""
    m = re.search(rf"^\s*{re.escape(name)}\s+(.*)$", out, re.M)
    return m.group(1).strip() if m else None


def parse_wifi(out: str) -> dict:
    r: dict = {"connected": False, "ip": "", "rssi": None, "hostname": "", "ssid": "", "time_synced": False,
               "stopped": False}
    host = _field(out, "hostname")
    if host:
        r["hostname"] = host.split()[0]
    net = _field(out, "network")
    if net and not net.startswith("(none"):
        r["ssid"] = net
    conn = _field(out, "connected") or ""
    m = re.match(r"yes, IP (\S+), RSSI (-?\d+) dBm", conn)
    if m:
        r.update(connected=True, ip=m.group(1), rssi=int(m.group(2)))
    r["stopped"] = "stopped for a transfer" in conn
    m = re.search(r"last disconnect reason (\d+)", conn)
    if m:
        r["last_disconnect_reason"] = int(m.group(1))
    rc = _field(out, "reconnects")
    if rc and rc.isdigit():
        r["reconnects"] = int(rc)
    r["time_synced"] = "synced (SNTP)" in (_field(out, "clock") or "")
    return r


_RUN_ROW = re.compile(r"^\s*(\d+)\s+(\S+)\s+(\d+)\.(\d)s\s+(\d+)\s+(yes|no|possibly)?\s*(.*)$")


def parse_runs(out: str) -> list[dict]:
    rows = []
    for line in out.splitlines():
        m = _RUN_ROW.match(line)
        if not m:
            continue
        rest = m.group(7).strip()
        version, _, error = rest.partition(" / ")
        rows.append({"seq": int(m.group(1)), "result": m.group(2), "seconds": int(m.group(3)) + int(m.group(4)) / 10,
                     "attempts": int(m.group(5)), "erased": m.group(6) or "", "version": version.strip(),
                     "error": error.strip()})
    return rows


def parse_run(out: str) -> tuple[Optional[dict], int]:
    """`run N` output -> (record JSON, log bytes)."""
    rec = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                rec = json.loads(line)
                break
            except json.JSONDecodeError:
                continue
    m = re.search(r"--- log \((\d+) bytes\) ---", out)
    return rec, int(m.group(1)) if m else 0


def parse_job(out: str) -> dict:
    r: dict = {"id": 0, "state": "", "phase": "", "error": "", "run": 0, "lease": ""}
    m = re.search(r"^\s*job\s+#(\d+) (\S+) \((\S+)\) for '([^']*)'", out, re.M)
    if m:
        r.update(id=int(m.group(1)), kind=m.group(2), source=m.group(3), target=m.group(4))
    state = _field(out, "state")
    if state:
        st, _, phase = state.partition(" / ")
        r["state"], r["phase"] = st.strip(), phase.strip()
    r["error"] = _field(out, "error") or ""
    m = re.search(r"^\s*run\s+#(\d+)", out, re.M)
    if m:
        r["run"] = int(m.group(1))
    m = re.search(r"^\s*progress\s+(\d+) / (\d+) B(?:, (\d+) B/s)?", out, re.M)
    if m:
        r.update(bytes_done=int(m.group(1)), bytes_total=int(m.group(2)), rate_bps=int(m.group(3) or 0))
    r["lease"] = _field(out, "lease") or ""
    r["target_erased"] = "ERASED" in (_field(out, "target") or "")
    return r


def parse_flash(out: str) -> dict:
    r: dict = {"ok": False, "aborted": False, "failed": False, "run": 0, "throughput_kBps": None,
               "attempts": [], "wifi_off": "WiFi is off until the flash finishes" in out,
               "wifi_restored": bool(re.search(r"^WiFi restored\.", out, re.M)), "error": "",
               "erased": "target's app was erased" in out}
    if re.search(r"^FLASH OK", out, re.M):
        r["ok"] = True
    m = re.search(r"^FLASH (FAILED|ABORTED) after (\d+) attempt\(s\): (.*)$", out, re.M)
    if m:
        r["failed"] = m.group(1) == "FAILED"
        r["aborted"] = m.group(1) == "ABORTED"
        r["error"] = m.group(3).strip()
    m = re.search(r"Recorded as run #(\d+)", out)
    if m:
        r["run"] = int(m.group(1))
    m = re.search(r"throughput\s+(\d+\.\d) kB/s", out)
    if m:
        r["throughput_kBps"] = float(m.group(1))
    for m in re.finditer(r"^\s*attempt (\d+)\s+interval (\d+) ms, PRN (\d+): (.*?)(?: \[(\w+), (\d+) B acked\])?$",
                         out, re.M):
        r["attempts"].append({"n": int(m.group(1)), "interval_ms": int(m.group(2)), "prn": int(m.group(3)),
                              "outcome": m.group(4).strip(), "kind": m.group(5) or "NONE"})
    if not r["ok"] and not r["error"]:
        # Refused before the flash started (no target, staged image bad, busy...).
        r["error"] = out.strip().splitlines()[-1] if out.strip() else ""
    return r


def parse_check(out: str) -> dict:
    r: dict = {"ok": "CHECK FAILED" not in out, "version": "", "board_match": False, "staged": ""}
    v = _field(out, "version")
    if v:
        r["version"] = v.split()[0]
    boards = _field(out, "boards") or ""
    r["board_match"] = boards.endswith(": match")
    r["staged"] = _field(out, "staged") or ""
    m = re.search(r"CHECK FAILED: (.*)", out)
    if m:
        r["error"] = m.group(1).strip()
    return r


def parse_stage(out: str) -> dict:
    r: dict = {"ok": "STAGE OK" in out, "reused": "already staged and re-verified" in out, "error": "",
               "invalidated": "previous staged image was invalidated" in out, "bytes": 0}
    m = re.search(r"STAGE FAILED: (.*)", out)
    if m:
        r["error"] = m.group(1).strip()
    m = re.search(r'STAGE OK: "[^"]*", (\d+) B in', out)
    if m:
        r["bytes"] = int(m.group(1))
    if not r["ok"] and not r["error"] and out.strip():
        r["error"] = out.strip().splitlines()[-1]
    return r


def parse_staged(out: str) -> dict:
    r: dict = {"valid": False, "sha256": "", "version": "", "board": "", "target": "", "verified": False}
    m = re.search(r"^\s*bin\s+(\d+) B\s+sha256 ([0-9a-f]{64})", out, re.M)
    if m:
        r.update(valid=True, size=int(m.group(1)), sha256=m.group(2))
    m = re.search(r'board\s+"([^"]*)"\s+version "([^"]*)"\s+target "([^"]*)"', out)
    if m:
        r.update(board=m.group(1), version=m.group(2), target=m.group(3))
    r["verified"] = bool(re.search(r"^\s*result\s+OK", out, re.M))
    return r


def parse_target(out: str) -> dict:
    r: dict = {}
    for key in ("id", "mac", "board", "manifest"):
        v = _field(out, key)
        if v is not None:
            r[key] = v
    if "mac" in r:
        m = re.match(r"([0-9A-Fa-f:]{17})", r["mac"])
        r["mac"] = m.group(1).upper() if m else ""
    if "id" in r:
        r["id"] = r["id"].split()[0]
    return r


def parse_token(out: str) -> str:
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    for i, ln in enumerate(lines):
        if ln.startswith("New API token") and i + 1 < len(lines):
            return lines[i + 1]
    return ""
