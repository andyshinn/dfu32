#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write an ESP Web Tools manifest for one DFU32 build (SPEC.md §9.4).

Reads an IDF build directory (flasher_args.json, project_description.json and
sdkconfig) and writes, under SITE:

  manifest-<board>.json
  fw/<version>/<board>/bootloader.bin, partition-table.bin, ota_data_initial.bin, dfu32.bin

The manifest lists the four parts at their flash offsets, never the merged
image: a merged .bin fills the gaps with 0xFF, so flashing it from the page
would wipe NVS (WiFi, token, target) and runlog. Writing the separate parts
leaves nvs, runlog and staging alone, and ota_data_initial.bin sends the next
boot to ota_0, which holds the new app.

The parts sit in a per-version directory because GitHub Pages lets browsers
cache files for 10 minutes: a manifest can never be paired with parts cached
from another build.

With --baseline, the board's partition table entry in that file (see
check_partitions.py) can carry an "erase_notice", which is copied into the
manifest as "dfu32_erase_notice" for the page to show above the button.

Example (the CI pages job runs this once per board):
  dfu32_web_manifest.py build/feather_esp32s3 --board feather_esp32s3 --site _site \\
      --baseline partitions/baseline.json
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from pathlib import Path

# flasher_args.json sections, in flash order. All four are required.
PARTS = ("bootloader", "partition-table", "otadata", "app")
BOARD_RE = re.compile(r"^[a-z0-9_]+$")


class ManifestError(Exception):
    pass


def chip_family(chip: str) -> str:
    """IDF target name to ESP Web Tools chipFamily: esp32 -> ESP32, esp32s3 -> ESP32-S3."""
    m = re.fullmatch(r"esp(32|8266)([a-z]\d+)?", chip.strip().lower())
    if not m:
        raise ManifestError(f"unknown chip '{chip}'")
    return f"ESP{m.group(1)}" + (f"-{m.group(2).upper()}" if m.group(2) else "")


def version_slug(version: str) -> str:
    """A version string as a single safe path component."""
    slug = re.sub(r"[^A-Za-z0-9._-]", "_", version).strip(".")
    if not slug:
        raise ManifestError(f"version '{version}' gives an empty directory name")
    return slug


def read_sdkconfig(path: Path) -> dict[str, str]:
    values = {}
    if not path.is_file():
        return values
    for line in path.read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, _, value = line.partition("=")
            values[key] = value.strip().strip('"')
    return values


def load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text())
    except FileNotFoundError:
        raise ManifestError(f"{path} not found") from None
    except json.JSONDecodeError as e:
        raise ManifestError(f"{path}: {e}") from None


def erase_notice(baseline: Path, sdkconfig: dict[str, str]) -> str | None:
    table = sdkconfig.get("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME")
    if not table:
        raise ManifestError("--baseline needs CONFIG_PARTITION_TABLE_CUSTOM_FILENAME in the build's sdkconfig")
    tables = load_json(baseline).get("tables", {})
    name = Path(table).name
    if name not in tables:
        raise ManifestError(f"{baseline} has no entry for {name}")
    notice = tables[name].get("erase_notice")
    if notice is not None and not (isinstance(notice, str) and notice.strip()):
        raise ManifestError(f"{baseline}: {name}.erase_notice must be null or non-empty text")
    return notice.strip() if notice else None


def build_manifest(build_dir: Path, board: str, site: Path, version: str | None = None,
                   baseline: Path | None = None) -> Path:
    """Copy the parts into SITE and write manifest-<board>.json. Returns the manifest path."""
    if not BOARD_RE.match(board):
        raise ManifestError(f"board '{board}' must be a profile name like feather_esp32s3")
    sdkconfig = read_sdkconfig(build_dir / "sdkconfig")
    if not sdkconfig:
        raise ManifestError(f"{build_dir / 'sdkconfig'} not found or empty")
    built_for = sdkconfig.get("CONFIG_DFU32_BOARD_NAME")
    if built_for != board:
        raise ManifestError(f"{build_dir} was built for board '{built_for}', not '{board}'")
    if sdkconfig.get("CONFIG_DFU32_DEBUG_ENDPOINTS") == "y":
        raise ManifestError(f"{build_dir} has the fault-injection endpoints; never publish a debug build")

    args = load_json(build_dir / "flasher_args.json")
    chip = args.get("extra_esptool_args", {}).get("chip")
    if not chip:
        raise ManifestError("flasher_args.json has no extra_esptool_args.chip")

    if version is None:
        version = load_json(build_dir / "project_description.json").get("project_version")
    if not version:
        raise ManifestError("no version: project_description.json has no project_version; pass --version")
    part_dir = Path("fw") / version_slug(version) / board

    parts = []
    for name in PARTS:
        entry = args.get(name)
        if not entry or "file" not in entry or "offset" not in entry:
            raise ManifestError(f"flasher_args.json has no '{name}' part")
        src = build_dir / entry["file"]
        if not src.is_file():
            raise ManifestError(f"{src} not found")
        rel = part_dir / src.name
        parts.append({"path": rel.as_posix(), "offset": int(str(entry["offset"]), 0), "src": src})
    offsets = [p["offset"] for p in parts]
    if len(set(offsets)) != len(offsets) or len({p["path"] for p in parts}) != len(parts):
        raise ManifestError("two parts share an offset or a file name")

    manifest = {
        "name": "DFU32",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{
            "chipFamily": chip_family(chip),
            "parts": [{"path": p["path"], "offset": p["offset"]} for p in parts],
        }],
    }
    if baseline is not None:
        notice = erase_notice(baseline, sdkconfig)
        if notice:
            manifest["dfu32_erase_notice"] = notice

    (site / part_dir).mkdir(parents=True, exist_ok=True)
    for p in parts:
        shutil.copyfile(p["src"], site / p["path"])
    out = site / f"manifest-{board}.json"
    out.write_text(json.dumps(manifest, indent=2) + "\n")
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build_dir", type=Path, help="IDF build directory, e.g. build/feather_esp32s3")
    ap.add_argument("--board", required=True, help="DFU32 board profile, e.g. feather_esp32s3")
    ap.add_argument("--site", required=True, type=Path, help="site directory to write into")
    ap.add_argument("--version", help="override project_description.json's project_version")
    ap.add_argument("--baseline", type=Path, help="partition baseline JSON with optional erase_notice")
    a = ap.parse_args(argv)
    try:
        out = build_manifest(a.build_dir, a.board, a.site, a.version, a.baseline)
    except ManifestError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    m = json.loads(out.read_text())
    print(f"{out}: {m['name']} {m['version']} ({m['builds'][0]['chipFamily']})")
    for p in m["builds"][0]["parts"]:
        print(f"  0x{p['offset']:06X}  {p['path']}")
    if "dfu32_erase_notice" in m:
        print(f"  erase notice: {m['dfu32_erase_notice']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
