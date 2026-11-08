#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds and checks DFU32's embedded web UI (SPEC.md §10.2).

    dfu32_ui.py build components/api/ui/index.html build/index.html.gz
    dfu32_ui.py check components/api/ui/index.html [--budget 40960]

`build` gzips the page deterministically (no timestamp or file name in the
header), so the firmware image doesn't change unless the UI does. `check`
prints the compressed size and fails above the budget (40 KB by default).
"""
import argparse
import gzip
import sys
from pathlib import Path

DEFAULT_BUDGET = 40 * 1024


def compress(data: bytes) -> bytes:
    # mtime=0 and no filename: byte-identical output for identical input.
    return gzip.compress(data, compresslevel=9, mtime=0)


def build(src: Path, out: Path) -> int:
    gz = compress(src.read_bytes())
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists() or out.read_bytes() != gz:
        out.write_bytes(gz)
    return len(gz)


def check(src: Path, budget: int) -> bool:
    raw = src.read_bytes()
    gz = compress(raw)
    pct = 100 * len(gz) / budget
    print(f"web UI: {len(raw)} B, {len(gz)} B gzipped ({pct:.0f}% of the {budget} B budget)")
    if len(gz) > budget:
        print(f"error: the gzipped UI exceeds its {budget} B budget (SPEC.md §10.2)", file=sys.stderr)
        return False
    return True


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="gzip the page for embedding")
    b.add_argument("src", type=Path)
    b.add_argument("out", type=Path)
    c = sub.add_parser("check", help="check the gzipped size against the budget")
    c.add_argument("src", type=Path)
    c.add_argument("--budget", type=int, default=DEFAULT_BUDGET)
    a = p.parse_args(argv)
    if a.cmd == "build":
        build(a.src, a.out)
        return 0
    return 0 if check(a.src, a.budget) else 1


if __name__ == "__main__":
    sys.exit(main())
