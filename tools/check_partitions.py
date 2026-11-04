#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate DFU32 partition CSVs against the invariants in SPEC.md §8 and §12.

Checks: no overlaps, required partitions present, app partitions 64 KB aligned,
data partitions 4 KB aligned, table fits the flash size, staging can hold a
4 KB header plus the largest nRF52840 application, runlog has enough slots.

Usage: check_partitions.py partitions/4mb.csv:4MB partitions/8mb.csv:8MB
"""
from __future__ import annotations

import sys
from pathlib import Path

KB = 1024
MB = 1024 * KB
STAGING_HEADER = 4 * KB
MAX_NRF52840_APP = 802816  # S140 6.1.1 (larger of the two SoftDevice cases)
MIN_RUNLOG_SLOTS = 10
FIRST_FREE = 0x9000  # after bootloader (0x0/0x1000) and partition table (0x8000)
REQUIRED = {"nvs", "otadata", "phy_init", "runlog", "ota_0", "ota_1", "staging", "coredump"}


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


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 2
    failed = False
    for arg in argv:
        csv, _, flash = arg.partition(":")
        flash_size = parse_size(flash or "4M")
        errs = check(Path(csv), flash_size)
        status = "OK" if not errs else "FAIL"
        print(f"{status}: {csv} (flash {flash_size // MB} MB)")
        for e in errs:
            print(f"  - {e}")
        failed |= bool(errs)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
