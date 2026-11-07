#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run idf.py for every board preset in CMakePresets.json, one after another.

With no arguments it runs `idf.py --preset <board> build` for each board.
This tool's options come first; everything from the first other argument on
is passed to idf.py in place of `build`, with "{preset}" replaced by the board
name. Put `--` before idf.py arguments that start with a dash:

    tools/build_all.py
    tools/build_all.py build merge-bin -o dfu32-{preset}-merged.bin
    tools/build_all.py --only feather_esp32s3 --only esp32s3_n16r8 fullclean build
    tools/build_all.py --only esp32s3_n16r8 -- -p /dev/ttyACM0 flash

Boards are built in sequence (each build already uses every CPU core). A failed
board doesn't stop the others; the exit status is 1 if any failed. The presets
are checked against boards/ first (see check_presets.py).
Activate ESP-IDF first so that idf.py is on PATH.

Usage: build_all.py [--only BOARD]... [--list [--json]] [--] [idf.py args...]
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_presets import check, load_presets  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parent.parent
Runner = Callable[[list[str]], int]


def run_idf(cmd: list[str]) -> int:
    return subprocess.run(cmd, cwd=PROJECT_DIR).returncode


def main(argv: list[str] | None = None, runner: Runner = run_idf, project_dir: Path = PROJECT_DIR) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        usage="%(prog)s [--only BOARD]... [--list [--json]] [--] [idf.py args...]",
        allow_abbrev=False,
    )
    ap.add_argument("--only", action="append", metavar="BOARD", help="limit to this preset (repeatable)")
    ap.add_argument("--list", action="store_true", help="print the preset names and exit")
    ap.add_argument("--json", action="store_true", help="with --list, print a JSON array")
    ap.add_argument("idf_args", nargs=argparse.REMAINDER, help="passed to idf.py (default: build)")
    args = ap.parse_args(argv)
    idf_args = args.idf_args[1:] if args.idf_args[:1] == ["--"] else args.idf_args

    errors = check(project_dir)
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    names = [p["name"] for p in load_presets(project_dir)]
    if args.only:
        unknown = [b for b in args.only if b not in names]
        if unknown:
            print(f"error: no preset named {', '.join(unknown)} (have: {', '.join(names)})", file=sys.stderr)
            return 2
        names = [n for n in names if n in args.only]

    if args.list:
        print(json.dumps(names) if args.json else "\n".join(names))
        return 0

    idf = "idf.py"
    if runner is run_idf:
        found = shutil.which("idf.py")
        if not found:
            print("error: idf.py not found on PATH; activate ESP-IDF v6.1 first", file=sys.stderr)
            return 2
        idf = found

    idf_args = idf_args or ["build"]
    results: dict[str, int] = {}
    for name in names:
        cmd = [idf, "--preset", name, *(a.replace("{preset}", name) for a in idf_args)]
        print(f"\n=== {name}: {' '.join(cmd[1:])}", flush=True)
        results[name] = runner(cmd)

    print("\n=== summary")
    for name, rc in results.items():
        print(f"  {name}: {'ok' if rc == 0 else f'FAILED (exit {rc})'}")
    return 0 if all(rc == 0 for rc in results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
