#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check that CMakePresets.json has exactly one preset per board profile.

Each boards/<name>/sdkconfig.defaults needs a configure preset named <name>
with binaryDir "build/<name>" and cacheVariables.DFU32_BOARD = "<name>", and
every preset must point at an existing board. idf.py ignores "inherits" (it
only warns), so presets must not use it.

Usage: check_presets.py [--project-dir .]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

PRESETS_FILE = "CMakePresets.json"


def load_presets(project_dir: Path) -> list[dict]:
    """Return the configure presets from <project_dir>/CMakePresets.json, in file order.

    Raises OSError if the file can't be read and ValueError if it isn't a JSON
    object with a "configurePresets" list.
    """
    data = json.loads((project_dir / PRESETS_FILE).read_text())
    if not isinstance(data, dict):
        raise ValueError("top level is not a JSON object")
    presets = data.get("configurePresets", [])
    if not isinstance(presets, list):
        raise ValueError('"configurePresets" is not a list')
    return presets


def cache_value(value: object) -> object:
    """Unwrap CMake's {"type": ..., "value": ...} cacheVariables form."""
    return value.get("value") if isinstance(value, dict) else value


def board_names(project_dir: Path) -> list[str]:
    return sorted(p.parent.name for p in (project_dir / "boards").glob("*/sdkconfig.defaults"))


def check(project_dir: Path) -> list[str]:
    """Return a list of problems; empty means presets and boards agree."""
    try:
        presets = load_presets(project_dir)
    except (OSError, ValueError) as e:
        return [f"cannot read {PRESETS_FILE}: {e}"]

    errors: list[str] = []
    boards = board_names(project_dir)
    if not presets:
        errors.append(f"{PRESETS_FILE} has no configure presets")
    seen: set[str] = set()
    for i, p in enumerate(presets):
        if not isinstance(p, dict):
            errors.append(f"configurePresets[{i}] is not an object")
            continue
        name = p.get("name")
        if not isinstance(name, str) or not name:
            errors.append(f"configurePresets[{i}] has no name")
            continue
        if name in seen:
            errors.append(f"duplicate preset '{name}'")
        seen.add(name)
        if name not in boards:
            errors.append(f"preset '{name}' has no boards/{name}/sdkconfig.defaults")
        if "inherits" in p:
            errors.append(f"preset '{name}' uses 'inherits', which idf.py does not support")
        if p.get("binaryDir") != f"build/{name}":
            errors.append(f"preset '{name}': binaryDir must be 'build/{name}', got {p.get('binaryDir')!r}")
        cache = p.get("cacheVariables")
        board = cache_value(cache.get("DFU32_BOARD")) if isinstance(cache, dict) else None
        if board != name:
            errors.append(f"preset '{name}': cacheVariables.DFU32_BOARD must be '{name}', got {board!r}")
    for b in boards:
        if b not in seen:
            errors.append(f"board '{b}' has no preset in {PRESETS_FILE}")
    return errors


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--project-dir", type=Path, default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args(argv)
    errors = check(args.project_dir)
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    if errors:
        return 1
    print(f"{PRESETS_FILE}: {len(board_names(args.project_dir))} board presets OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
