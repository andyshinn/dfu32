#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate DFU32 partition CSVs against the invariants in SPEC.md §8 and §12.

Checks: no overlaps, required partitions present, app partitions 64 KB aligned,
data partitions 4 KB aligned, table fits the flash size, staging can hold a
4 KB header plus the largest nRF52840 application, runlog has enough slots.

With --baseline, also checks that nvs, runlog and staging keep the offsets and
sizes recorded in that file (SPEC §9.4). Settings survive a browser-installer
update only while those three stay put, so a change there is breaking: update
the baseline in the same PR (--write-baseline) and set that table's
"erase_notice", which the installer page shows above its button.

Usage:
  check_partitions.py partitions/4mb.csv:4MB partitions/8mb.csv:8MB
  check_partitions.py --baseline partitions/baseline.json partitions/4mb.csv:4MB partitions/8mb.csv:8MB
  check_partitions.py --write-baseline partitions/baseline.json partitions/4mb.csv:4MB partitions/8mb.csv:8MB
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

KB = 1024
MB = 1024 * KB
STAGING_HEADER = 4 * KB
MAX_NRF52840_APP = 802816  # S140 6.1.1 (larger of the two SoftDevice cases)
MIN_RUNLOG_SLOTS = 10
FIRST_FREE = 0x9000  # after bootloader (0x0/0x1000) and partition table (0x8000)
REQUIRED = {"nvs", "otadata", "phy_init", "runlog", "ota_0", "ota_1", "staging", "coredump"}
# Partitions whose contents a keep-settings update must find where it left them.
KEPT = ("nvs", "runlog", "staging")


def parse_size(text: str) -> int:
    t = text.strip().upper()
    if t.endswith("K"):
        return int(t[:-1], 0) * KB
    if t.endswith("M"):
        return int(t[:-1], 0) * MB
    return int(t, 0)


def load(path: Path) -> list[dict]:
    rows = []
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 5:
            raise ValueError(f"{path}:{lineno}: expected >= 5 columns")
        name, ptype, subtype, offset, size = cols[:5]
        if not offset:
            raise ValueError(f"{path}:{lineno}: DFU32 tables must give explicit offsets")
        rows.append({"name": name, "type": ptype, "subtype": subtype,
                     "offset": parse_size(offset), "size": parse_size(size), "line": lineno})
    return rows


def check(path: Path, flash_size: int) -> list[str]:
    errors = []
    rows = load(path)
    names = {r["name"] for r in rows}
    for missing in sorted(REQUIRED - names):
        errors.append(f"missing partition '{missing}'")

    prev_end = FIRST_FREE
    for r in sorted(rows, key=lambda r: r["offset"]):
        align = 0x10000 if r["type"] == "app" else 0x1000
        if r["offset"] % align:
            errors.append(f"{r['name']}: offset 0x{r['offset']:X} not aligned to 0x{align:X}")
        if r["size"] % 0x1000:
            errors.append(f"{r['name']}: size 0x{r['size']:X} not a multiple of 4 KB")
        if r["offset"] < prev_end:
            errors.append(f"{r['name']}: overlaps previous partition (starts 0x{r['offset']:X} < 0x{prev_end:X})")
        prev_end = max(prev_end, r["offset"] + r["size"])
    if prev_end > flash_size:
        errors.append(f"table ends at 0x{prev_end:X}, beyond flash size 0x{flash_size:X}")

    by_name = {r["name"]: r for r in rows}
    if "staging" in by_name:
        need = STAGING_HEADER + MAX_NRF52840_APP
        if by_name["staging"]["size"] < need:
            errors.append(f"staging: {by_name['staging']['size']} B < header + max app ({need} B)")
    if "runlog" in by_name and by_name["runlog"]["size"] // (4 * KB) < MIN_RUNLOG_SLOTS:
        errors.append(f"runlog: fewer than {MIN_RUNLOG_SLOTS} x 4 KB slots")
    if "ota_0" in by_name and "ota_1" in by_name and by_name["ota_0"]["size"] != by_name["ota_1"]["size"]:
        errors.append("ota_0 and ota_1 differ in size")
    return errors


def kept_layout(rows: list[dict]) -> dict:
    by_name = {r["name"]: r for r in rows}
    return {n: {"offset": f"0x{by_name[n]['offset']:X}", "size": f"0x{by_name[n]['size']:X}"}
            for n in KEPT if n in by_name}


def check_baseline(path: Path, baseline: dict) -> list[str]:
    entry = baseline.get("tables", {}).get(path.name)
    if entry is None:
        return [f"no baseline for {path.name}; add one with --write-baseline"]
    want, have = entry.get("partitions", {}), kept_layout(load(path))
    errors = []
    for name in KEPT:
        w, h = want.get(name), have.get(name)
        if w is None:
            errors.append(f"{name}: not in the baseline for {path.name}")
        elif h is not None and (int(w["offset"], 0), int(w["size"], 0)) != (int(h["offset"], 0), int(h["size"], 0)):
            errors.append(f"{name}: {h['offset']} size {h['size']} differs from the baseline "
                          f"{w['offset']} size {w['size']}. Updates that keep settings would break. "
                          f"If deliberate, update the baseline (--write-baseline) and set erase_notice (SPEC §9.4)")
    return errors


def write_baseline(path: Path, tables: list[Path]) -> None:
    """Record the kept partitions of each table, keeping any erase_notice already set."""
    data = json.loads(path.read_text()) if path.exists() else {}
    out = {"tables": dict(data.get("tables", {}))}
    for t in tables:
        old = out["tables"].get(t.name, {})
        out["tables"][t.name] = {"partitions": kept_layout(load(t)), "erase_notice": old.get("erase_notice")}
    out["tables"] = dict(sorted(out["tables"].items()))
    path.write_text(json.dumps(out, indent=2) + "\n")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tables", nargs="*", metavar="CSV[:FLASH]", help="partition CSV, with flash size (default 4M)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--baseline", type=Path, help="also check nvs/runlog/staging against this file")
    g.add_argument("--write-baseline", type=Path, help="record nvs/runlog/staging of the given tables in this file")
    a = ap.parse_args(argv)
    if not a.tables:
        ap.print_help()
        return 2
    baseline = json.loads(a.baseline.read_text()) if a.baseline else None
    failed = False
    for arg in a.tables:
        csv, _, flash = arg.partition(":")
        flash_size = parse_size(flash or "4M")
        errs = check(Path(csv), flash_size)
        if baseline is not None:
            errs += check_baseline(Path(csv), baseline)
        status = "OK" if not errs else "FAIL"
        print(f"{status}: {csv} (flash {flash_size // MB} MB)")
        for e in errs:
            print(f"  - {e}")
        failed |= bool(errs)
    if a.write_baseline and not failed:
        write_baseline(a.write_baseline, [Path(t.partition(":")[0]) for t in a.tables])
        print(f"wrote {a.write_baseline}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
