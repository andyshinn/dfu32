#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Add earlier releases to the installer site, so the page can install any of them (SPEC.md §9.4).

The pages job builds the site for one version: this run's build, written by
dfu32_web_manifest.py as manifest-<board>.json. This tool adds the published
releases next to it. Their merged images (the dfu32-<board>-merged.bin
release assets) can't be flashed as they are: a merged image fills the gaps
between parts with 0xFF, so it would wipe NVS and runlog. So each one is split
back into its four parts (bootloader, partition table, otadata, app), found
the way the ROM bootloader and ESP-IDF find them: the bootloader at the chip's
fixed offset, the partition table at 0x8000, and otadata and the first app
partition from that table. Each part is cut to the length its image header
gives, so nothing past it is written.

For each release and board it writes:

  fw/<version>/<board>/bootloader.bin, partition-table.bin, ota_data_initial.bin, dfu32.bin
  fw/<version>/<board>/manifest.json   (part paths relative to it)

and then SITE/versions.json, which the page reads to fill its version list:

  {"default": "<version>", "versions": [
    {"version": ..., "channel": "main" | "release" | "pre-release", "tag": ..., "published_at": ...,
     "manifests": {"<board>": "<path>", ...}}, ...]}

The first entry is this run's build (manifest-<board>.json); the others are
releases, newest first. The version is the one the app reports
(esp_app_desc_t), so the page shows the same string DFU32's own page shows.

A release whose nvs, runlog or staging partition sits somewhere other than in
this run's build for the same board gets a "dfu32_erase_notice", which the
page shows above its button: moving between the two layouts without an erase
would leave settings where the other firmware doesn't look.

A release or board that can't be split (a missing asset, an image that isn't
DFU32's or names another board) is left out with a warning; it never fails the
deploy.

Example (the CI pages job, after dfu32_web_manifest.py):
  gh release list --json tagName,isPrerelease,isDraft,publishedAt > releases.json
  gh release download <tag> -p 'dfu32-*-merged.bin' -D releases/<tag>    # for each tag
  dfu32_web_versions.py --site _site --releases releases.json --assets releases --channel main
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_partitions import KEPT  # noqa: E402
from dfu32_web_manifest import BOARD_RE, version_slug  # noqa: E402

IMAGE_MAGIC = 0xE9
PARTITION_TABLE_OFFSET = 0x8000  # CONFIG_PARTITION_TABLE_OFFSET
PARTITION_TABLE_SIZE = 0xC00     # what partition-table.bin holds
APP_DESC_OFFSET = 24 + 8         # image header + first segment header
APP_DESC_MAGIC = 0xABCD5432
BOARD_TAG_OFFSET = APP_DESC_OFFSET + 256
BOARD_TAG_MAGIC = b"DF32BRD1"
# esp_image_header_t.chip_id -> ESP Web Tools chipFamily and the bootloader's flash offset.
CHIPS = {
    0x0000: ("ESP32", 0x1000),
    0x0002: ("ESP32-S2", 0x1000),
    0x0005: ("ESP32-C3", 0x0),
    0x0009: ("ESP32-S3", 0x0),
    0x000C: ("ESP32-C2", 0x0),
    0x000D: ("ESP32-C6", 0x0),
    0x0010: ("ESP32-H2", 0x0),
}
ASSET_RE = re.compile(r"^dfu32-([a-z0-9_]+)-merged\.bin$")
SEMVER_PRE = re.compile(r"^v?\d+\.\d+\.\d+-[0-9A-Za-z.-]+$")
PART_FILES = ("bootloader.bin", "partition-table.bin", "ota_data_initial.bin", "dfu32.bin")


class SplitError(Exception):
    pass


def cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def image_length(data: bytes, off: int) -> int:
    """Length of the ESP-IDF image (bootloader or app) at OFF: header, segments,
    the checksum byte at the end of the next 16-byte block, and the SHA-256 if appended."""
    if off + 24 > len(data) or data[off] != IMAGE_MAGIC:
        raise SplitError(f"no image at 0x{off:X}")
    count = data[off + 1]
    if not 1 <= count <= 16:
        raise SplitError(f"image at 0x{off:X} has {count} segments")
    p = off + 24
    for _ in range(count):
        if p + 8 > len(data):
            raise SplitError(f"image at 0x{off:X} is truncated")
        (size,) = struct.unpack_from("<I", data, p + 4)
        p += 8 + size
    p += 16 - (p - off) % 16  # the checksum byte ends the block
    if data[off + 23] == 1:
        p += 32
    if p > len(data):
        raise SplitError(f"image at 0x{off:X} is truncated")
    return p - off


def partitions(table: bytes) -> list[dict]:
    """Entries of an ESP-IDF binary partition table."""
    out = []
    for p in range(0, len(table) - 31, 32):
        magic, ptype, subtype, offset, size = struct.unpack_from("<HBBII", table, p)
        if magic != 0x50AA:
            break  # 0xEBEB is the MD5 entry, 0xFFFF the end
        out.append({"label": cstr(table[p + 12:p + 28]), "type": ptype, "subtype": subtype,
                    "offset": offset, "size": size})
    return out


def kept_layout(table: bytes) -> dict[str, tuple[int, int]]:
    """Where nvs, runlog and staging sit: the partitions a keep-settings update must find in place."""
    return {e["label"]: (e["offset"], e["size"]) for e in partitions(table) if e["label"] in KEPT}


def app_info(app: bytes) -> dict:
    """The version and project from esp_app_desc_t, and DFU32's board tag ('' if absent)."""
    if len(app) < BOARD_TAG_OFFSET + 40 or struct.unpack_from("<I", app, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC:
        raise SplitError("the app has no esp_app_desc_t")
    tag = app[BOARD_TAG_OFFSET:BOARD_TAG_OFFSET + 40]
    return {"version": cstr(app[APP_DESC_OFFSET + 16:APP_DESC_OFFSET + 48]),
            "project": cstr(app[APP_DESC_OFFSET + 48:APP_DESC_OFFSET + 80]),
            "board": cstr(tag[8:]) if tag[:8] == BOARD_TAG_MAGIC else ""}


def split_merged(merged: bytes) -> dict:
    """The four parts of a merged image, at their flash offsets, plus the chip family and app info."""
    boot_off = next((o for o in (0x0, 0x1000) if len(merged) > o + 24 and merged[o] == IMAGE_MAGIC), None)
    if boot_off is None:
        raise SplitError("no bootloader at 0x0 or 0x1000")
    (chip_id,) = struct.unpack_from("<H", merged, boot_off + 12)
    if chip_id not in CHIPS:
        raise SplitError(f"unknown chip id {chip_id}")
    family, expected = CHIPS[chip_id]
    if boot_off != expected:
        raise SplitError(f"{family} bootloader at 0x{boot_off:X}, expected 0x{expected:X}")
    boot = merged[boot_off:boot_off + image_length(merged, boot_off)]
    if boot_off + len(boot) > PARTITION_TABLE_OFFSET:
        raise SplitError("the bootloader runs into the partition table")

    table = merged[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE]
    entries = partitions(table)
    if not entries:
        raise SplitError("no partition table at 0x8000")
    otadata = next((e for e in entries if e["type"] == 1 and e["subtype"] == 0), None)
    # The partition flasher_args.json names as "app": factory if there is one, else ota_0.
    app_part = next((e for e in entries if e["type"] == 0 and e["subtype"] == 0x00), None) or \
        next((e for e in entries if e["type"] == 0 and e["subtype"] == 0x10), None)
    if not otadata or not app_part:
        raise SplitError("the partition table has no otadata or no ota_0")
    if otadata["offset"] + otadata["size"] > len(merged):
        raise SplitError("the image ends before otadata")
    app = merged[app_part["offset"]:app_part["offset"] + image_length(merged, app_part["offset"])]
    if len(app) > app_part["size"]:
        raise SplitError("the app is larger than its partition")
    return {
        "chip_family": family,
        "parts": [(boot_off, boot), (PARTITION_TABLE_OFFSET, table),
                  (otadata["offset"], merged[otadata["offset"]:otadata["offset"] + otadata["size"]]),
                  (app_part["offset"], app)],
        "app": app_info(app),
    }


def default_entry(site: Path, channel: str, tag: str | None) -> dict:
    """This run's build: the manifest-<board>.json files dfu32_web_manifest.py wrote."""
    manifests, versions = {}, set()
    for path in sorted(site.glob("manifest-*.json")):
        board = path.stem[len("manifest-"):]
        if BOARD_RE.match(board):
            manifests[board] = path.name
            versions.add(json.loads(path.read_text())["version"])
    if len(versions) != 1:
        raise SplitError(f"{site} should hold manifest-<board>.json files of one version, found {sorted(versions)}")
    return {"version": versions.pop(), "channel": channel, "tag": tag, "published_at": None, "manifests": manifests}


def default_table(site: Path, manifest: str) -> bytes | None:
    m = json.loads((site / manifest).read_text())
    part = next((p for p in m["builds"][0]["parts"] if p["offset"] == PARTITION_TABLE_OFFSET), None)
    return (site / part["path"]).read_bytes() if part else None


def add_release(site: Path, release: dict, assets: Path, default: dict, warn) -> dict | None:
    """Split one release's merged images into SITE. Returns its versions.json entry, or None."""
    tag = release["tagName"]
    pre = bool(release.get("isPrerelease")) or bool(SEMVER_PRE.match(tag))
    entry = {"version": None, "channel": "pre-release" if pre else "release", "tag": tag,
             "published_at": release.get("publishedAt"), "manifests": {}}
    files = sorted((assets / tag).glob("dfu32-*-merged.bin")) if (assets / tag).is_dir() else []
    for path in files:
        m = ASSET_RE.match(path.name)
        if not m:
            continue
        board = m.group(1)
        if board not in default["manifests"]:
            continue  # a board the page no longer offers
        try:
            split = split_merged(path.read_bytes())
        except (SplitError, struct.error) as e:
            warn(f"{tag} {board}: {e}")
            continue
        info = split["app"]
        if info["project"] != "dfu32" or (info["board"] and info["board"] != board):
            warn(f"{tag} {board}: the image is {info['project']!r} for {info['board']!r}")
            continue
        if entry["version"] is None:
            entry["version"] = info["version"]
        elif entry["version"] != info["version"]:
            warn(f"{tag} {board}: version {info['version']} differs from {entry['version']}")
            continue
        if entry["version"] == default["version"]:
            continue  # this run's own build of the same version takes its place
        rel = Path("fw") / version_slug(entry["version"]) / board
        manifest = {
            "name": "DFU32",
            "version": entry["version"],
            "new_install_prompt_erase": True,
            "builds": [{"chipFamily": split["chip_family"],
                        "parts": [{"path": name, "offset": off}
                                  for name, (off, _) in zip(PART_FILES, split["parts"])]}],
        }
        current = default_table(site, default["manifests"][board])
        if current is not None and kept_layout(current) != kept_layout(split["parts"][1][1]):
            manifest["dfu32_erase_notice"] = (
                f"{entry['version']} keeps its settings partitions somewhere other than {default['version']}, "
                "so moving between the two needs “Erase device”, which removes WiFi, the API token and the target.")
        out = site / rel
        if out.exists():
            warn(f"{tag} {board}: {rel} already exists")
            continue
        out.mkdir(parents=True)
        for name, (_, data) in zip(PART_FILES, split["parts"]):
            (out / name).write_bytes(data)
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        entry["manifests"][board] = (rel / "manifest.json").as_posix()
    if entry["version"] == default["version"]:
        # Pushed to main at the tag, or this run is the release: one entry, labelled as the release.
        default["tag"] = default["tag"] or tag
        if default["channel"] == "main":
            default["channel"] = entry["channel"]
        return None
    return entry if entry["manifests"] else None


def build_index(site: Path, releases: list[dict], assets: Path, channel: str, tag: str | None = None,
                limit: int = 20, warn=lambda msg: print(f"warning: {msg}", file=sys.stderr)) -> dict:
    default = default_entry(site, channel, tag)
    published = sorted((r for r in releases if not r.get("isDraft")),
                       key=lambda r: r.get("publishedAt") or "", reverse=True)[:limit]
    entries, seen = [default], {default["version"]}
    for release in published:
        e = add_release(site, release, assets, default, warn)
        if e and e["version"] not in seen:
            seen.add(e["version"])
            entries.append(e)
    index = {"default": default["version"], "versions": entries}
    (site / "versions.json").write_text(json.dumps(index, indent=2) + "\n")
    return index


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--site", required=True, type=Path, help="site directory holding manifest-<board>.json")
    ap.add_argument("--releases", required=True, type=Path,
                    help="JSON from gh release list --json tagName,isPrerelease,isDraft,publishedAt")
    ap.add_argument("--assets", required=True, type=Path, help="directory of <tag>/dfu32-<board>-merged.bin")
    ap.add_argument("--channel", choices=("main", "release"), default="main", help="what this run's build is")
    ap.add_argument("--tag", help="this run's release tag, if it is one")
    ap.add_argument("--limit", type=int, default=20, help="newest releases to offer (default 20)")
    a = ap.parse_args(argv)
    try:
        releases = json.loads(a.releases.read_text())
        index = build_index(a.site, releases, a.assets, a.channel, a.tag, a.limit)
    except (SplitError, OSError, json.JSONDecodeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    for e in index["versions"]:
        print(f"{e['version']}  {e['channel']}{' ' + e['tag'] if e['tag'] else ''}  {', '.join(sorted(e['manifests']))}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
