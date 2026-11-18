#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Describe a DFU32 release and every binary in it: dfu32-release.json (SPEC.md §9.4).

Each release carries this file as an asset. It says what was built (version,
tag, commit, ESP-IDF), and for each board the chip, flash size, partition
table, where nvs/runlog/staging sit, the erase notice, and each binary's size
and SHA-256. The installer site is built from these files, not from the
binaries themselves.

The four flash parts (bootloader, partition table, otadata, app) aren't
separate assets: each is a byte range of the board's merged image, which holds
every part at its flash offset. A part's entry gives that offset, its size and
its SHA-256, so anyone can cut it out of dfu32-<board>-merged.bin and check it
(`extract`). Whether a release is stable or a pre-release isn't recorded here:
that is the GitHub release's flag, which can change after the build.

  {"schema": 1, "project": "dfu32", "source": "build" | "backfill",
   "version": "v0.6.0", "tag": "v0.6.0", "commit": "<sha>" | null,
   "built_at": "<ISO 8601>" | null, "idf_version": "v6.1" | null,
   "boards": {"<board>": {
     "chip_family": "ESP32-S3", "flash_size": "4MB" | null, "partition_table": "4mb.csv" | null,
     "kept": {"nvs": [offset, size], "runlog": [...], "staging": [...]},
     "erase_notice": "<text>" | null, "elf_sha256": "<hex>" | null,
     "merged": {"asset": "dfu32-<board>-merged.bin", "size": n, "sha256": "<hex>"},
     "app": {"asset": "dfu32-<board>.bin", "size": n, "sha256": "<hex>"} | null,
     "parts": [{"role": "bootloader" | "partition-table" | "otadata" | "app",
                "offset": n, "size": n, "sha256": "<hex>"}, ...]}}}

Offsets and sizes are integers. Parts are listed in flash order.

Commands:
  build       from IDF build directories, in CI (each holds dfu32-<board>-merged.bin from merge-bin):
                dfu32_release_manifest.py build --tag v0.6.0 --commit $GITHUB_SHA \\
                    --baseline partitions/baseline.json --out dfu32-release.json \\
                    feather_esp32s3=build/feather_esp32s3 feather_esp32_v2=build/feather_esp32_v2
  from-merged for a release published before this file existed, from its assets
              (the parts are found the way dfu32_web_versions.py finds them):
                dfu32_release_manifest.py from-merged --tag v0.5.2 --assets assets/v0.5.2 --out dfu32-release.json
  verify      check a manifest against a directory of release assets
  extract     verify, then write each part to OUT/sha256/<hash>.bin (what the installer site serves)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_partitions import KEPT  # noqa: E402
from dfu32_web_manifest import BOARD_RE, PARTS, ManifestError, chip_family, erase_notice, load_json, read_sdkconfig  # noqa: E402
from dfu32_web_versions import (APP_DESC_OFFSET, ASSET_RE, SplitError, app_info, cstr, kept_layout,  # noqa: E402
                                split_merged)

SCHEMA = 1
ROLES = PARTS  # bootloader, partition-table, otadata, app (flasher_args.json's names)
# esp_image_header_t byte 3, high nibble: the flash size the bootloader was built for.
FLASH_SIZES = {0: "1MB", 1: "2MB", 2: "4MB", 3: "8MB", 4: "16MB", 5: "32MB", 6: "64MB", 7: "128MB"}


class ReleaseError(Exception):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def blob(asset: str, data: bytes) -> dict:
    return {"asset": asset, "size": len(data), "sha256": sha256(data)}


def merged_name(board: str) -> str:
    return f"dfu32-{board}-merged.bin"


def app_name(board: str) -> str:
    return f"dfu32-{board}.bin"


def app_extra(app: bytes) -> dict:
    """ESP-IDF version and ELF SHA-256 from esp_app_desc_t (idf_ver at +112, app_elf_sha256 at +144)."""
    idf = cstr(app[APP_DESC_OFFSET + 112:APP_DESC_OFFSET + 144])
    elf = app[APP_DESC_OFFSET + 144:APP_DESC_OFFSET + 176]
    return {"idf_version": idf or None, "elf_sha256": elf.hex() if any(elf) else None}


def flash_size_of(bootloader: bytes) -> str | None:
    return FLASH_SIZES.get(bootloader[3] >> 4) if len(bootloader) > 3 else None


def kept_json(layout: dict[str, tuple[int, int]]) -> dict[str, list[int]]:
    return {name: list(layout[name]) for name in KEPT if name in layout}


def baseline_match(baseline: dict, layout: dict[str, tuple[int, int]]) -> tuple[str | None, str | None]:
    """The partition CSV whose nvs, runlog and staging match LAYOUT, and its erase notice.

    A merged image doesn't name its CSV, so a backfilled release finds it by layout.
    No match, or two tables with the same layout, gives (None, None)."""
    found = []
    for name, entry in baseline.get("tables", {}).items():
        kept = {k: (int(str(p["offset"]), 0), int(str(p["size"]), 0))
                for k, p in entry.get("partitions", {}).items() if k in KEPT}
        if kept == layout:
            found.append((name, (entry.get("erase_notice") or "").strip() or None))
    return found[0] if len(found) == 1 else (None, None)


def finish(boards: dict, version: str, tag: str | None, commit: str | None, built_at: str | None,
           idf: set, source: str) -> dict:
    if not boards:
        raise ReleaseError("no boards")
    return {
        "schema": SCHEMA, "project": "dfu32", "source": source,
        "version": version, "tag": tag, "commit": commit, "built_at": built_at,
        "idf_version": idf.pop() if len(idf) == 1 else None,
        "boards": dict(sorted(boards.items())),
    }


# ---- build ----

def build_board(board: str, build_dir: Path, merged_path: Path, baseline: Path | None) -> tuple[dict, dict]:
    """One board's entry from its IDF build directory, and its app info."""
    if not BOARD_RE.match(board):
        raise ReleaseError(f"board '{board}' must be a profile name like feather_esp32s3")
    sdk = read_sdkconfig(build_dir / "sdkconfig")
    if sdk.get("CONFIG_DFU32_BOARD_NAME") != board:
        raise ReleaseError(f"{build_dir} was built for board '{sdk.get('CONFIG_DFU32_BOARD_NAME')}', not '{board}'")
    if sdk.get("CONFIG_DFU32_DEBUG_ENDPOINTS") == "y":
        raise ReleaseError(f"{build_dir} has the fault-injection endpoints; never release a debug build")
    try:
        args = load_json(build_dir / "flasher_args.json")
        chip = chip_family(args.get("extra_esptool_args", {}).get("chip", ""))
        notice = erase_notice(baseline, sdk) if baseline else None
    except ManifestError as e:
        raise ReleaseError(f"{board}: {e}") from None
    if not merged_path.is_file():
        raise ReleaseError(f"{board}: {merged_path} not found (idf.py merge-bin)")
    merged = merged_path.read_bytes()

    parts, data = [], {}
    for role in ROLES:
        entry = args.get(role)
        if not entry or "file" not in entry or "offset" not in entry:
            raise ReleaseError(f"{board}: flasher_args.json has no '{role}' part")
        src = build_dir / entry["file"]
        if not src.is_file():
            raise ReleaseError(f"{board}: {src} not found")
        data[role] = src.read_bytes()
        off = int(str(entry["offset"]), 0)
        # The site cuts each part out of the merged image, so it must be there, byte for byte.
        if merged[off:off + len(data[role])] != data[role]:
            raise ReleaseError(f"{board}: {merged_path.name} doesn't hold {src.name} at 0x{off:X}")
        parts.append({"role": role, "offset": off, "size": len(data[role]), "sha256": sha256(data[role])})
    parts.sort(key=lambda p: p["offset"])
    check_parts(board, parts, len(merged))

    try:
        info = app_info(data["app"])
    except SplitError as e:
        raise ReleaseError(f"{board}: {e}") from None
    if info["project"] != "dfu32" or (info["board"] and info["board"] != board):
        raise ReleaseError(f"{board}: the app is {info['project']!r} for {info['board']!r}")
    extra = app_extra(data["app"])
    table = sdk.get("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME")
    return {
        "chip_family": chip,
        "flash_size": sdk.get("CONFIG_ESPTOOLPY_FLASHSIZE") or flash_size_of(data["bootloader"]),
        "partition_table": Path(table).name if table else None,
        "kept": kept_json(kept_layout(data["partition-table"])),
        "erase_notice": notice,
        "elf_sha256": extra["elf_sha256"],
        "merged": blob(merged_name(board), merged),
        "app": blob(app_name(board), data["app"]),
        "parts": parts,
    }, {**info, **extra}


def build(boards: dict[str, Path], tag: str | None = None, commit: str | None = None, built_at: str | None = None,
          baseline: Path | None = None, merged: dict[str, Path] | None = None) -> dict:
    """The manifest for BOARDS (board -> IDF build directory). With TAG, every app must report it as its version."""
    if not boards:
        raise ReleaseError("no boards")
    entries, versions, idf = {}, set(), set()
    for board, build_dir in boards.items():
        path = (merged or {}).get(board) or build_dir / merged_name(board)
        entries[board], info = build_board(board, build_dir, path, baseline)
        versions.add(info["version"])
        if info["idf_version"]:
            idf.add(info["idf_version"])
    if len(versions) != 1:
        raise ReleaseError(f"the boards were built at different versions: {sorted(versions)}")
    version = versions.pop()
    if tag and version != tag:
        raise ReleaseError(f"the apps report version {version!r}, not the tag {tag}")
    return finish(entries, version, tag, commit, built_at, idf, "build")


# ---- from-merged (backfill) ----

def from_merged(assets: Path, tag: str, commit: str | None = None, baseline: dict | None = None,
                warn=lambda msg: print(f"warning: {msg}", file=sys.stderr)) -> dict:
    """The manifest for a published release, from its dfu32-<board>-merged.bin assets in ASSETS.

    A board whose image can't be split, or isn't DFU32's for that board, is left out with a
    warning, as is one built at another version than the rest; a release with none fails."""
    entries, versions, idf = {}, set(), set()
    for path in sorted(assets.glob("dfu32-*-merged.bin")):
        m = ASSET_RE.match(path.name)
        if not m:
            continue
        board, merged = m.group(1), path.read_bytes()
        try:
            split = split_merged(merged)
        except (SplitError, struct.error) as e:
            warn(f"{tag} {board}: {e}")
            continue
        info = split["app"]
        if info["project"] != "dfu32" or (info["board"] and info["board"] != board):
            warn(f"{tag} {board}: the image is {info['project']!r} for {info['board']!r}")
            continue
        if versions and info["version"] not in versions:
            warn(f"{tag} {board}: version {info['version']} differs from {next(iter(versions))}")
            continue
        versions.add(info["version"])
        app = split["parts"][3][1]
        extra = app_extra(app)
        if extra["idf_version"]:
            idf.add(extra["idf_version"])
        parts = [{"role": role, "offset": off, "size": len(data), "sha256": sha256(data)}
                 for role, (off, data) in zip(ROLES, split["parts"])]
        parts.sort(key=lambda p: p["offset"])
        layout = kept_layout(split["parts"][1][1])
        table, notice = baseline_match(baseline or {}, layout)
        app_path = assets / app_name(board)
        app_blob = None
        if app_path.is_file():
            if app_path.read_bytes() != app:
                warn(f"{tag} {board}: {app_path.name} differs from the app in {path.name}; left out")
            else:
                app_blob = blob(app_path.name, app)
        entries[board] = {
            "chip_family": split["chip_family"],
            "flash_size": flash_size_of(split["parts"][0][1]),
            "partition_table": table,
            "kept": kept_json(layout),
            "erase_notice": notice,
            "elf_sha256": extra["elf_sha256"],
            "merged": blob(path.name, merged),
            "app": app_blob,
            "parts": parts,
        }
    if not entries:
        raise ReleaseError(f"{tag}: no usable dfu32-<board>-merged.bin in {assets}")
    version = versions.pop()
    if version != tag:
        warn(f"{tag}: the apps report version {version!r}")
    return finish(entries, version, tag, commit, None, idf, "backfill")


# ---- verify / extract ----

def validate(manifest: dict) -> None:
    """Check the manifest's shape, so a consumer can rely on it."""
    if manifest.get("schema") != SCHEMA:
        raise ReleaseError(f"schema {manifest.get('schema')!r}, expected {SCHEMA}")
    if manifest.get("project") != "dfu32" or not isinstance(manifest.get("version"), str) or not manifest["version"]:
        raise ReleaseError("not a DFU32 release manifest (project, version)")
    boards = manifest.get("boards")
    if not isinstance(boards, dict) or not boards:
        raise ReleaseError("no boards")
    for board, b in boards.items():
        if not BOARD_RE.match(board):
            raise ReleaseError(f"bad board name {board!r}")
        if not isinstance(b.get("merged"), dict) or b["merged"].get("asset") != merged_name(board):
            raise ReleaseError(f"{board}: merged must name {merged_name(board)}")
        parts = b.get("parts")
        if not isinstance(parts, list) or sorted(p.get("role") for p in parts) != sorted(ROLES):
            raise ReleaseError(f"{board}: parts must be exactly {', '.join(ROLES)}")
        for p in parts:
            if not all(isinstance(p.get(k), int) and p[k] >= 0 for k in ("offset", "size")) or \
                    not isinstance(p.get("sha256"), str) or len(p["sha256"]) != 64:
                raise ReleaseError(f"{board}: {p.get('role')}: needs integer offset and size and a SHA-256")
        if [p["offset"] for p in parts] != sorted(p["offset"] for p in parts):
            raise ReleaseError(f"{board}: parts must be in flash order")
        check_parts(board, parts, b["merged"].get("size"))


def check_parts(board: str, parts: list[dict], merged_size) -> None:
    for a, b in zip(parts, parts[1:]):
        if a["offset"] + a["size"] > b["offset"]:
            raise ReleaseError(f"{board}: {a['role']} overlaps {b['role']}")
    if not isinstance(merged_size, int) or (parts and parts[-1]["offset"] + parts[-1]["size"] > merged_size):
        raise ReleaseError(f"{board}: the parts run past the end of the merged image")


def verify(manifest: dict, assets: Path, boards: list[str] | None = None) -> dict[str, dict[str, bytes]]:
    """Check each board's assets in ASSETS against MANIFEST. Returns board -> {part sha256: bytes}."""
    validate(manifest)
    unknown = sorted(set(boards or ()) - set(manifest["boards"]))
    if unknown:
        raise ReleaseError(f"not in the manifest: {', '.join(unknown)}")
    out = {}
    for board, b in manifest["boards"].items():
        if boards is not None and board not in boards:
            continue
        path = assets / b["merged"]["asset"]
        if not path.is_file():
            raise ReleaseError(f"{board}: {path} not found")
        merged = path.read_bytes()
        if len(merged) != b["merged"]["size"] or sha256(merged) != b["merged"]["sha256"]:
            raise ReleaseError(f"{board}: {path.name} doesn't match the manifest")
        out[board] = {}
        for p in b["parts"]:
            data = merged[p["offset"]:p["offset"] + p["size"]]
            if sha256(data) != p["sha256"]:
                raise ReleaseError(f"{board}: the {p['role']} at 0x{p['offset']:X} doesn't match the manifest")
            out[board][p["sha256"]] = data
        if b.get("app"):
            app = assets / b["app"]["asset"]
            if app.is_file() and sha256(app.read_bytes()) != b["app"]["sha256"]:
                raise ReleaseError(f"{board}: {app.name} doesn't match the manifest")
    return out


def extract(manifest: dict, assets: Path, out: Path, boards: list[str] | None = None) -> list[Path]:
    """Verify, then write each part to OUT/sha256/<hash>.bin. Returns the paths written."""
    written = []
    for parts in verify(manifest, assets, boards).values():
        for digest, data in parts.items():
            dest = out / "sha256" / f"{digest}.bin"
            dest.parent.mkdir(parents=True, exist_ok=True)
            if not dest.exists():
                dest.write_bytes(data)
            written.append(dest)
    return written


# ---- CLI ----

def board_dirs(items: list[str]) -> dict[str, Path]:
    out = {}
    for item in items:
        board, sep, path = item.partition("=")
        if not sep or not board or not path:
            raise ReleaseError(f"expected BOARD=PATH, got {item!r}")
        if board in out:
            raise ReleaseError(f"{board} given twice")
        out[board] = Path(path)
    return out


def write(manifest: dict, out: Path) -> None:
    validate(manifest)
    out.write_text(json.dumps(manifest, indent=2) + "\n")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="from IDF build directories")
    b.add_argument("boards", nargs="+", metavar="BOARD=BUILD_DIR")
    b.add_argument("--tag", help="the release tag; every app must report it as its version")
    b.add_argument("--commit")
    b.add_argument("--built-at", help="ISO 8601 time of the build")
    b.add_argument("--baseline", type=Path, help="partition baseline JSON (erase_notice)")
    b.add_argument("--merged", action="append", default=[], metavar="BOARD=PATH",
                   help="merged image, if not BUILD_DIR/dfu32-<board>-merged.bin")
    b.add_argument("--out", type=Path, required=True)
    m = sub.add_parser("from-merged", help="from a published release's merged images (backfill)")
    m.add_argument("--tag", required=True)
    m.add_argument("--assets", type=Path, required=True, help="directory holding the release's assets")
    m.add_argument("--commit")
    m.add_argument("--baseline", type=Path, help="the partition baseline at that tag")
    m.add_argument("--out", type=Path, required=True)
    for name, helptext in (("verify", "check a manifest against release assets"),
                           ("extract", "verify, then write parts to OUT/sha256/<hash>.bin")):
        p = sub.add_parser(name, help=helptext)
        p.add_argument("manifest", type=Path)
        p.add_argument("--assets", type=Path, required=True)
        p.add_argument("--board", action="append", help="only this board (repeatable)")
        if name == "extract":
            p.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "build":
            manifest = build(board_dirs(a.boards), a.tag, a.commit, a.built_at, a.baseline, board_dirs(a.merged))
            write(manifest, a.out)
        elif a.cmd == "from-merged":
            baseline = json.loads(a.baseline.read_text()) if a.baseline else None
            manifest = from_merged(a.assets, a.tag, a.commit, baseline)
            write(manifest, a.out)
        else:
            manifest = json.loads(a.manifest.read_text())
            if a.cmd == "verify":
                verify(manifest, a.assets, a.board)
            else:
                print(f"{len(set(extract(manifest, a.assets, a.out, a.board)))} parts in {a.out / 'sha256'}")
    except (ReleaseError, OSError, json.JSONDecodeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"{manifest['version']} ({manifest.get('source', '?')}): " + ", ".join(
        f"{board} {b['chip_family']}" for board, b in manifest["boards"].items()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
