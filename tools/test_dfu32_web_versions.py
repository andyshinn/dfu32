#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for dfu32_web_versions.py: splitting release images and writing versions.json.

Run: python3 -m unittest discover -s tools -p 'test_*.py'
"""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dfu32_web_manifest as wm  # noqa: E402
import dfu32_web_versions as wv  # noqa: E402
from test_dfu32_web_manifest import flasher_args  # noqa: E402

CHIP_S3, CHIP_ESP32, CHIP_C5 = 9, 0, 0x17
BOOT_OFFSET = {CHIP_ESP32: 0x1000, CHIP_C5: 0x2000}  # 0x0 otherwise
# (label, type, subtype, offset, size) like partitions/4mb.csv and 8mb.csv.
TABLE_4MB = [("nvs", 1, 2, 0x9000, 0x6000), ("otadata", 1, 0, 0xF000, 0x2000), ("phy_init", 1, 1, 0x11000, 0x1000),
             ("runlog", 1, 0x41, 0x12000, 0xE000), ("ota_0", 0, 0x10, 0x20000, 0x170000),
             ("ota_1", 0, 0x11, 0x190000, 0x170000), ("staging", 1, 0x40, 0x300000, 0xD0000),
             ("coredump", 1, 3, 0x3D0000, 0x10000)]
TABLE_8MB = [("nvs", 1, 2, 0x9000, 0xF000), ("otadata", 1, 0, 0x18000, 0x2000), ("phy_init", 1, 1, 0x1A000, 0x1000),
             ("runlog", 1, 0x41, 0x1B000, 0x10000), ("ota_0", 0, 0x10, 0x30000, 0x300000),
             ("ota_1", 0, 0x11, 0x330000, 0x300000), ("staging", 1, 0x40, 0x630000, 0x100000),
             ("coredump", 1, 3, 0x730000, 0x10000)]


def partition_table(entries) -> bytes:
    """ESP-IDF binary partition table: 32-byte entries, the MD5 entry, then 0xFF to 0xC00."""
    raw = b"".join(struct.pack("<HBBII16sI", 0x50AA, t, st, off, size, label.encode(), 0)
                   for label, t, st, off, size in entries)
    raw += b"\xEB\xEB" + b"\xFF" * 14 + hashlib.md5(raw).digest()
    return raw + b"\xFF" * (0xC00 - len(raw))


def esp_image(chip_id: int, segments: list[bytes], hash_appended: bool = True) -> bytes:
    """An ESP-IDF image: header, segments, checksum byte at the end of a 16-byte block, SHA-256."""
    out = bytearray(24)
    out[0], out[1] = 0xE9, len(segments)
    struct.pack_into("<H", out, 12, chip_id)
    out[23] = 1 if hash_appended else 0
    for i, data in enumerate(segments):
        out += struct.pack("<II", 0x3C000000 + i * 0x10000, len(data)) + data
    out += b"\0" * (15 - len(out) % 16) + b"\xEF"
    if hash_appended:
        out += hashlib.sha256(out).digest()
    return bytes(out)


def app_image(chip_id: int = CHIP_S3, version: str = "v0.4.0", board: str | None = "feather_esp32s3",
              project: str = "dfu32", size: int = 3000) -> bytes:
    desc = struct.pack("<II8x32s32s", 0xABCD5432, 0, version.encode(), project.encode()).ljust(256, b"\0")
    tag = (b"DF32BRD1" + board.encode().ljust(32, b"\0")) if board is not None else b"\0" * 40
    first = desc + tag + bytes(range(256)) * 2
    return esp_image(chip_id, [first, bytes((i * 13) & 0xFF for i in range(size))])


def merged_image(chip_id: int = CHIP_S3, table=TABLE_4MB, app: bytes | None = None,
                 boot: bytes | None = None) -> tuple[bytes, list[tuple[int, bytes]]]:
    """A merged image as idf.py merge-bin writes it (0xFF between parts), and the parts in it."""
    boot_off = BOOT_OFFSET.get(chip_id, 0x0)
    boot = boot or esp_image(chip_id, [b"\x11" * 5000, b"\x22" * 700])
    app = app or app_image(chip_id)
    otadata = next(e for e in table if e[0] == "otadata")
    ota0 = next(e for e in table if e[0] == "ota_0")
    parts = [(boot_off, boot), (0x8000, partition_table(table)), (otadata[3], b"\xFF" * otadata[4]), (ota0[3], app)]
    out = bytearray(b"\xFF" * (ota0[3] + len(app)))
    for off, data in parts:
        out[off:off + len(data)] = data
    return bytes(out), parts


class ImageLength(unittest.TestCase):
    def test_with_and_without_hash(self):
        for h in (True, False):
            img = esp_image(CHIP_S3, [b"a" * 33, b"b" * 7], hash_appended=h)
            self.assertEqual(wv.image_length(img + b"\xFF" * 100, 0), len(img))
            self.assertEqual(len(img) % 16, 0)

    def test_at_an_offset(self):
        img = esp_image(CHIP_ESP32, [b"x" * 21])
        self.assertEqual(wv.image_length(b"\xFF" * 0x1000 + img, 0x1000), len(img))

    def test_truncated(self):
        img = esp_image(CHIP_S3, [b"a" * 100])
        with self.assertRaisesRegex(wv.SplitError, "truncated"):
            wv.image_length(img[:-1], 0)

    def test_not_an_image(self):
        with self.assertRaisesRegex(wv.SplitError, "no image"):
            wv.image_length(b"\xFF" * 64, 0)


class Split(unittest.TestCase):
    def test_s3_4mb(self):
        merged, parts = merged_image(CHIP_S3, TABLE_4MB)
        s = wv.split_merged(merged)
        self.assertEqual(s["chip_family"], "ESP32-S3")
        self.assertEqual(s["parts"], parts)
        self.assertEqual(s["app"], {"version": "v0.4.0", "project": "dfu32", "board": "feather_esp32s3"})

    def test_classic_esp32_8mb(self):
        app = app_image(CHIP_ESP32, board="feather_esp32_v2")
        merged, parts = merged_image(CHIP_ESP32, TABLE_8MB, app=app)
        s = wv.split_merged(merged)
        self.assertEqual(s["chip_family"], "ESP32")
        self.assertEqual([o for o, _ in s["parts"]], [0x1000, 0x8000, 0x18000, 0x30000])
        self.assertEqual(s["parts"], parts)

    def test_c5_8mb(self):
        app = app_image(CHIP_C5, board="xiao_esp32c5")
        merged, parts = merged_image(CHIP_C5, TABLE_8MB, app=app)
        s = wv.split_merged(merged)
        self.assertEqual(s["chip_family"], "ESP32-C5")
        self.assertEqual([o for o, _ in s["parts"]], [0x2000, 0x8000, 0x18000, 0x30000])
        self.assertEqual(s["parts"], parts)

    def test_factory_partition_wins(self):
        table = [("nvs", 1, 2, 0x9000, 0x6000), ("otadata", 1, 0, 0xF000, 0x2000),
                 ("factory", 0, 0x00, 0x20000, 0x100000), ("ota_0", 0, 0x10, 0x120000, 0x100000)]
        app = app_image()
        merged = bytearray(merged_image(CHIP_S3, TABLE_4MB)[0][:0x8000]) + partition_table(table)
        merged += b"\xFF" * (0x20000 - len(merged)) + app
        self.assertEqual(wv.split_merged(bytes(merged))["parts"][3], (0x20000, app))

    def test_untagged_app(self):
        merged, _ = merged_image(app=app_image(board=None))
        self.assertEqual(wv.split_merged(merged)["app"]["board"], "")

    def test_bootloader_at_the_wrong_offset_for_its_chip(self):
        merged, _ = merged_image(CHIP_S3)
        moved = b"\xFF" * 0x1000 + merged[:0x7000] + merged[0x8000:]  # an S3 bootloader at 0x1000
        with self.assertRaisesRegex(wv.SplitError, "expected 0x0"):
            wv.split_merged(moved)

    def test_no_bootloader(self):
        with self.assertRaisesRegex(wv.SplitError, "no bootloader"):
            wv.split_merged(b"\xFF" * 0x10000)

    def test_unknown_chip(self):
        merged, _ = merged_image(boot=esp_image(0x77, [b"z" * 64]))
        with self.assertRaisesRegex(wv.SplitError, "unknown chip"):
            wv.split_merged(merged)

    def test_no_partition_table(self):
        merged = bytearray(merged_image()[0])
        merged[0x8000:0x8C00] = b"\xFF" * 0xC00
        with self.assertRaisesRegex(wv.SplitError, "no partition table"):
            wv.split_merged(bytes(merged))

    def test_app_cut_short(self):
        merged, _ = merged_image()
        with self.assertRaisesRegex(wv.SplitError, "truncated"):
            wv.split_merged(merged[:-10])

    def test_app_larger_than_its_partition(self):
        small = [e if e[0] != "ota_0" else ("ota_0", 0, 0x10, 0x20000, 0x1000) for e in TABLE_4MB]
        merged, _ = merged_image(table=small, app=app_image(size=0x1000))
        with self.assertRaisesRegex(wv.SplitError, "larger than its partition"):
            wv.split_merged(merged)

    def test_parts_never_point_at_the_bootloader_or_table(self):
        for name, off in (("otadata", 0x8000), ("otadata", 0x0), ("ota_0", 0x0), ("ota_0", 0x8000), ("ota_0", 0x28000)):
            with self.subTest(part=name, offset=off):
                table = [e if e[0] != name else (name, e[1], e[2], off, e[4]) for e in TABLE_4MB]
                merged = bytearray(merged_image()[0])
                merged[0x8000:0x8C00] = partition_table(table)
                with self.assertRaisesRegex(wv.SplitError, "overlaps|64 KB boundary"):
                    wv.split_merged(bytes(merged))

    def test_otadata_running_into_the_app(self):
        table = [e if e[0] != "otadata" else ("otadata", 1, 0, 0xF000, 0x11800) for e in TABLE_4MB]
        merged = bytearray(merged_image()[0])
        merged[0x8000:0x8C00] = partition_table(table)
        with self.assertRaisesRegex(wv.SplitError, "overlap"):
            wv.split_merged(bytes(merged))

    def test_app_without_a_descriptor(self):
        merged, _ = merged_image(app=esp_image(CHIP_S3, [b"\0" * 400]))
        with self.assertRaisesRegex(wv.SplitError, "esp_app_desc_t"):
            wv.split_merged(merged)

    def test_kept_layout(self):
        self.assertEqual(wv.kept_layout(partition_table(TABLE_4MB)),
                         {"nvs": (0x9000, 0x6000), "runlog": (0x12000, 0xE000), "staging": (0x300000, 0xD0000)})


class Site:
    """A site with this run's build (via dfu32_web_manifest.py) and a directory of release assets."""

    def __init__(self, version="v0.5.0-3-gabc1234", boards=(("feather_esp32s3", "esp32s3", TABLE_4MB),
                                                              ("feather_esp32_v2", "esp32", TABLE_8MB))):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.site, self.assets = root / "site", root / "assets"
        self.version = version
        for board, chip, table in boards:
            build = root / "build" / board
            (build / "bootloader").mkdir(parents=True)
            (build / "partition_table").mkdir()
            cid = CHIP_ESP32 if chip == "esp32" else CHIP_S3
            merged, parts = merged_image(cid, table, app=app_image(cid, version, board))
            for (_, data), rel in zip(parts, ("bootloader/bootloader.bin", "partition_table/partition-table.bin",
                                              "ota_data_initial.bin", "dfu32.bin")):
                (build / rel).write_bytes(data)
            otadata = next(e for e in table if e[0] == "otadata")[3]
            ota0 = next(e for e in table if e[0] == "ota_0")[3]
            boot = "0x1000" if chip == "esp32" else "0x0"
            (build / "flasher_args.json").write_text(json.dumps(flasher_args(chip, boot, hex(ota0), hex(otadata))))
            (build / "project_description.json").write_text(json.dumps({"project_version": version}))
            (build / "sdkconfig").write_text(f'CONFIG_DFU32_BOARD_NAME="{board}"\n')
            wm.build_manifest(build, board, self.site)

    def release(self, tag, version=None, boards=(("feather_esp32s3", CHIP_S3, TABLE_4MB),), **app):
        d = self.assets / tag
        d.mkdir(parents=True, exist_ok=True)
        for board, chip, table in boards:
            img = app_image(chip, version or tag, **{"board": board, **app})
            (d / f"dfu32-{board}-merged.bin").write_bytes(merged_image(chip, table, app=img)[0])

    def index(self, releases, channel="main", tag=None, limit=20):
        self.warnings = []
        return wv.build_index(self.site, releases, self.assets, channel, tag, limit, warn=self.warnings.append)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.tmp.cleanup()


def rel(tag, when, pre=False, draft=False):
    return {"tagName": tag, "publishedAt": when, "isPrerelease": pre, "isDraft": draft}


class Index(unittest.TestCase):
    def test_default_only(self):
        with Site() as s:
            ix = s.index([])
            self.assertEqual(ix["default"], s.version)
            self.assertEqual(ix["versions"], [{"version": s.version, "channel": "main", "tag": None, "published_at": None,
                                                "manifests": {"feather_esp32_v2": "manifest-feather_esp32_v2.json",
                                                              "feather_esp32s3": "manifest-feather_esp32s3.json"}}])
            self.assertEqual(json.loads((s.site / "versions.json").read_text()), ix)

    def test_releases_newest_first(self):
        with Site() as s:
            s.release("v0.4.0", boards=(("feather_esp32s3", CHIP_S3, TABLE_4MB), ("feather_esp32_v2", CHIP_ESP32, TABLE_8MB)))
            s.release("v0.4.1-rc.1")
            s.release("v0.3.0")
            ix = s.index([rel("v0.3.0", "2026-01-01T00:00:00Z"), rel("v0.4.0", "2026-03-01T00:00:00Z"),
                          rel("v0.4.1-rc.1", "2026-04-01T00:00:00Z")])
            self.assertEqual([(e["version"], e["channel"]) for e in ix["versions"]],
                             [(s.version, "main"), ("v0.4.1-rc.1", "pre-release"), ("v0.4.0", "release"), ("v0.3.0", "release")])
            v040 = ix["versions"][2]
            self.assertEqual(v040["manifests"], {"feather_esp32s3": "fw/v0.4.0/feather_esp32s3/manifest.json",
                                                 "feather_esp32_v2": "fw/v0.4.0/feather_esp32_v2/manifest.json"})
            self.assertEqual(v040["published_at"], "2026-03-01T00:00:00Z")
            self.assertEqual(s.warnings, [])

    def test_manifest_and_parts(self):
        with Site() as s:
            s.release("v0.4.0", boards=(("feather_esp32_v2", CHIP_ESP32, TABLE_8MB),))
            s.index([rel("v0.4.0", "2026-03-01T00:00:00Z")])
            d = s.site / "fw" / "v0.4.0" / "feather_esp32_v2"
            m = json.loads((d / "manifest.json").read_text())
            self.assertEqual(m["name"], "DFU32")
            self.assertEqual(m["version"], "v0.4.0")
            self.assertTrue(m["new_install_prompt_erase"])
            self.assertNotIn("dfu32_erase_notice", m)  # same layout as this run's build
            self.assertEqual(m["builds"][0]["chipFamily"], "ESP32")
            parts = m["builds"][0]["parts"]
            self.assertEqual([(p["path"], p["offset"]) for p in parts],
                             [("bootloader.bin", 0x1000), ("partition-table.bin", 0x8000),
                              ("ota_data_initial.bin", 0x18000), ("dfu32.bin", 0x30000)])
            _, expected = merged_image(CHIP_ESP32, TABLE_8MB, app=app_image(CHIP_ESP32, "v0.4.0", "feather_esp32_v2"))
            for p, (_, data) in zip(parts, expected):
                self.assertEqual((d / p["path"]).read_bytes(), data)

    def test_prerelease_flag_or_semver_suffix(self):
        with Site() as s:
            s.release("v0.4.0-rc.1")
            s.release("v0.4.0b")
            ix = s.index([rel("v0.4.0-rc.1", "2026-03-01T00:00:00Z"), rel("v0.4.0b", "2026-03-02T00:00:00Z", pre=True)])
            self.assertEqual([e["channel"] for e in ix["versions"][1:]], ["pre-release", "pre-release"])

    def test_drafts_and_missing_assets_left_out(self):
        with Site() as s:
            s.release("v0.4.0")
            ix = s.index([rel("v0.4.0", "2026-03-01T00:00:00Z", draft=True), rel("v0.3.0", "2026-02-01T00:00:00Z")])
            self.assertEqual(len(ix["versions"]), 1)

    def test_limit(self):
        with Site() as s:
            for i in range(5):
                s.release(f"v0.{i}.0")
            ix = s.index([rel(f"v0.{i}.0", f"2026-0{i + 1}-01T00:00:00Z") for i in range(5)], limit=2)
            self.assertEqual([e["version"] for e in ix["versions"][1:]], ["v0.4.0", "v0.3.0"])

    def test_version_comes_from_the_app(self):
        with Site() as s:
            s.release("v0.4.0", version="v0.4.0-dirty")
            ix = s.index([rel("v0.4.0", "2026-03-01T00:00:00Z")])
            self.assertEqual((ix["versions"][1]["version"], ix["versions"][1]["tag"]), ("v0.4.0-dirty", "v0.4.0"))

    def test_this_runs_version_is_folded_into_the_default(self):
        with Site(version="v0.5.0") as s:
            s.release("v0.5.0")
            ix = s.index([rel("v0.5.0", "2026-03-01T00:00:00Z")])
            self.assertEqual(len(ix["versions"]), 1)
            self.assertEqual((ix["versions"][0]["channel"], ix["versions"][0]["tag"]), ("release", "v0.5.0"))
            self.assertFalse((s.site / "fw" / "v0.5.0" / "feather_esp32s3" / "manifest.json").exists())

    def test_release_run(self):
        with Site(version="v0.5.0") as s:
            ix = s.index([], channel="release", tag="v0.5.0")
            self.assertEqual((ix["versions"][0]["channel"], ix["versions"][0]["tag"]), ("release", "v0.5.0"))

    def test_release_run_of_a_prerelease(self):
        # By its tag, or by the flag in releases.json even before its assets are attached.
        with Site(version="v0.5.0-rc.1") as s:
            ix = s.index([], channel="release", tag="v0.5.0-rc.1")
            self.assertEqual(ix["versions"][0]["channel"], "pre-release")
        with Site(version="v0.5.0b") as s:
            ix = s.index([rel("v0.5.0b", "2026-03-01T00:00:00Z", pre=True)], channel="release", tag="v0.5.0b")
            self.assertEqual((ix["versions"][0]["channel"], len(ix["versions"])), ("pre-release", 1))

    def test_main_at_a_prerelease_tag(self):
        with Site(version="v0.5.0-rc.1") as s:
            s.release("v0.5.0-rc.1")
            ix = s.index([rel("v0.5.0-rc.1", "2026-03-01T00:00:00Z")])
            self.assertEqual((ix["versions"][0]["channel"], ix["versions"][0]["tag"]), ("pre-release", "v0.5.0-rc.1"))

    def test_other_board_or_project_skipped(self):
        with Site() as s:
            s.release("v0.4.0", board="esp32s3_n16r8")  # asset named feather_esp32s3, app tagged n16r8
            s.release("v0.3.0", project="other")
            ix = s.index([rel("v0.4.0", "2026-03-01T00:00:00Z"), rel("v0.3.0", "2026-02-01T00:00:00Z")])
            self.assertEqual(len(ix["versions"]), 1)
            self.assertEqual(len(s.warnings), 2)
            self.assertIn("esp32s3_n16r8", s.warnings[0])

    def test_untagged_app_trusted_by_its_asset_name(self):
        with Site() as s:
            s.release("v0.3.0", board=None)
            d = s.assets / "v0.3.0"
            (d / "dfu32-feather_esp32s3-merged.bin").write_bytes(
                merged_image(CHIP_S3, TABLE_4MB, app=app_image(CHIP_S3, "v0.3.0", board=None))[0])
            ix = s.index([rel("v0.3.0", "2026-02-01T00:00:00Z")])
            self.assertEqual(list(ix["versions"][1]["manifests"]), ["feather_esp32s3"])

    def test_board_not_on_the_site_skipped(self):
        with Site() as s:
            s.release("v0.4.0", boards=(("old_board", CHIP_S3, TABLE_4MB),))
            ix = s.index([rel("v0.4.0", "2026-03-01T00:00:00Z")])
            self.assertEqual(len(ix["versions"]), 1)

    def test_unsplittable_image_warns(self):
        with Site() as s:
            (s.assets / "v0.4.0").mkdir(parents=True)
            (s.assets / "v0.4.0" / "dfu32-feather_esp32s3-merged.bin").write_bytes(b"\xFF" * 0x9000)
            ix = s.index([rel("v0.4.0", "2026-03-01T00:00:00Z")])
            self.assertEqual(len(ix["versions"]), 1)
            self.assertIn("no bootloader", s.warnings[0])

    def test_moved_settings_partition_gets_an_erase_notice(self):
        moved = [e if e[0] != "nvs" else ("nvs", 1, 2, 0x9000, 0x5000) for e in TABLE_4MB]
        with Site() as s:
            s.release("v0.4.0", boards=(("feather_esp32s3", CHIP_S3, moved),))
            s.index([rel("v0.4.0", "2026-03-01T00:00:00Z")])
            m = json.loads((s.site / "fw" / "v0.4.0" / "feather_esp32s3" / "manifest.json").read_text())
            self.assertIn("Erase device", m["dfu32_erase_notice"])
            self.assertIn(s.version, m["dfu32_erase_notice"])

    def test_two_versions_of_this_run_fail(self):
        with Site() as s:
            (s.site / "manifest-other.json").write_text(json.dumps({"version": "v9"}))
            with self.assertRaisesRegex(wv.SplitError, "one version"):
                s.index([])

    def test_cli(self):
        with Site() as s:
            s.release("v0.4.0")
            releases = Path(s.tmp.name) / "releases.json"
            releases.write_text(json.dumps([rel("v0.4.0", "2026-03-01T00:00:00Z")]))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(wv.main(["--site", str(s.site), "--releases", str(releases), "--assets", str(s.assets)]), 0)
            self.assertIn("v0.4.0  release v0.4.0  feather_esp32s3", out.getvalue())
            with contextlib.redirect_stderr(io.StringIO()) as err:
                self.assertEqual(wv.main(["--site", str(s.site), "--releases", str(releases) + ".missing",
                                          "--assets", str(s.assets)]), 1)
            self.assertIn("error:", err.getvalue())


if __name__ == "__main__":
    unittest.main()
