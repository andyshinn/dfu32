#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail if the DFU32 app binary exceeds a budget fraction of its OTA slot.

Reads <build-dir>/sdkconfig to find the partition CSV, takes the ota_0 size as
the slot size, and compares it with <build-dir>/dfu32.bin (SPEC.md §12.1: the
4 MB layout's 1.44 MB slots get a 90% budget, about 1.30 MB).

Usage: check_app_size.py <build-dir> [--budget 0.90] [--project-dir .]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_partitions import load  # noqa: E402


def sdkconfig_value(path: Path, key: str) -> str | None:
    pattern = re.compile(rf'^{re.escape(key)}="?(.*?)"?$')
    for line in path.read_text().splitlines():
        m = pattern.match(line.strip())
        if m:
            return m.group(1)
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("build_dir", type=Path)
    ap.add_argument("--budget", type=float, default=0.90)
    ap.add_argument("--project-dir", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("--app", default="dfu32.bin")
    args = ap.parse_args()

    sdkconfig = args.build_dir / "sdkconfig"
    csv_name = sdkconfig_value(sdkconfig, "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME")
    if not csv_name:
        print(f"error: CONFIG_PARTITION_TABLE_CUSTOM_FILENAME not found in {sdkconfig}")
        return 2
    rows = {r["name"]: r for r in load(args.project_dir / csv_name)}
    slot = rows["ota_0"]["size"]
    app = (args.build_dir / args.app).stat().st_size
    limit = int(slot * args.budget)
    pct = 100.0 * app / slot
    print(f"app {app} B / slot {slot} B ({pct:.1f}%), budget {limit} B ({args.budget:.0%}) [{csv_name}]")
    if app > limit:
        print("error: app exceeds size budget (see SPEC.md §12.1 for levers)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
