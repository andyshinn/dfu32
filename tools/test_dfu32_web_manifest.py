#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for dfu32_web_manifest.py and the check_partitions.py baseline.

Run: python3 -m unittest discover -s tools -p 'test_*.py'
"""
from __future__ import annotations

import contextlib
import io
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_partitions as cp  # noqa: E402
import dfu32_web_manifest as wm  # noqa: E402
import fetch_esp_web_tools as fewt  # noqa: E402

REPO = Path(__file__).resolve().parent.parent


def flasher_args(chip: str = "esp32s3", boot: str = "0x0", app: str = "0x20000", otadata: str = "0xf000") -> dict:
    """The parts of an IDF flasher_args.json that the tool reads."""
    return {
        "flash_files": {boot: "bootloader/bootloader.bin", "0x8000": "partition_table/partition-table.bin",
                        otadata: "ota_data_initial.bin", app: "dfu32.bin"},
        "bootloader": {"offset": boot, "file": "bootloader/bootloader.bin", "encrypted": "false"},
        "partition-table": {"offset": "0x8000", "file": "partition_table/partition-table.bin", "encrypted": "false"},
        "otadata": {"offset": otadata, "file": "ota_data_initial.bin", "encrypted": "false"},
        "app": {"offset": app, "file": "dfu32.bin", "encrypted": "false"},
        "extra_esptool_args": {"after": "hard_reset", "before": "default_reset", "stub": True, "chip": chip},
    }


class Build:
    """A fake IDF build directory plus an empty site directory."""

    def __init__(self, board="feather_esp32s3", chip="esp32s3", version="v0.3.0-4-gabc1234",
                 table="partitions/4mb.csv", debug=False, args=None):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.dir, self.site = root / "build", root / "site"
        (self.dir / "bootloader").mkdir(parents=True)
        (self.dir / "partition_table").mkdir()
        for rel, data in (("bootloader/bootloader.bin", b"boot"), ("partition_table/partition-table.bin", b"pt"),
                          ("ota_data_initial.bin", b"ota"), ("dfu32.bin", b"app")):
            (self.dir / rel).write_bytes(data)
        (self.dir / "flasher_args.json").write_text(json.dumps(args or flasher_args(chip)))
        if version is not None:
            (self.dir / "project_description.json").write_text(json.dumps({"project_version": version}))
        lines = [f'CONFIG_IDF_TARGET="{chip}"', f'CONFIG_DFU32_BOARD_NAME="{board}"',
                 f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{table}"']
        lines.append("CONFIG_DFU32_DEBUG_ENDPOINTS=y" if debug else "# CONFIG_DFU32_DEBUG_ENDPOINTS is not set")
        (self.dir / "sdkconfig").write_text("\n".join(lines) + "\n")

    def baseline(self, notices: dict) -> Path:
        path = Path(self.tmp.name) / "baseline.json"
        path.write_text(json.dumps({"tables": {name: {"partitions": {}, "erase_notice": n}
                                               for name, n in notices.items()}}))
        return path

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.tmp.cleanup()


class ChipFamily(unittest.TestCase):
    def test_names(self):
        for chip, family in (("esp32", "ESP32"), ("esp32s3", "ESP32-S3"), ("esp32c3", "ESP32-C3"),
                             ("esp32c61", "ESP32-C61"), ("ESP32S2", "ESP32-S2"), ("esp8266", "ESP8266")):
            self.assertEqual(wm.chip_family(chip), family)

    def test_unknown(self):
        with self.assertRaises(wm.ManifestError):
            wm.chip_family("rp2040")


class VersionSlug(unittest.TestCase):
    def test_describe_strings_unchanged(self):
        for v in ("v0.5.0", "v0.3.0-12-gabc1234", "3f2a1bc", "v0.3.0-12-gabc1234-dirty"):
            self.assertEqual(wm.version_slug(v), v)

    def test_unsafe_characters(self):
        self.assertEqual(wm.version_slug("../v1 /x"), "_v1__x")
        with self.assertRaises(wm.ManifestError):
            wm.version_slug("..")


class Manifest(unittest.TestCase):
    def test_s3(self):
        with Build() as b:
            out = wm.build_manifest(b.dir, "feather_esp32s3", b.site)
            self.assertEqual(out, b.site / "manifest-feather_esp32s3.json")
            m = json.loads(out.read_text())
            self.assertEqual(m, {
                "name": "DFU32",
                "version": "v0.3.0-4-gabc1234",
                "new_install_prompt_erase": True,
                "builds": [{"chipFamily": "ESP32-S3", "parts": [
                    {"path": "fw/v0.3.0-4-gabc1234/feather_esp32s3/bootloader.bin", "offset": 0x0},
                    {"path": "fw/v0.3.0-4-gabc1234/feather_esp32s3/partition-table.bin", "offset": 0x8000},
                    {"path": "fw/v0.3.0-4-gabc1234/feather_esp32s3/ota_data_initial.bin", "offset": 0xF000},
                    {"path": "fw/v0.3.0-4-gabc1234/feather_esp32s3/dfu32.bin", "offset": 0x20000},
                ]}],
            })
            for p, data in zip(m["builds"][0]["parts"], (b"boot", b"pt", b"ota", b"app")):
                self.assertEqual((b.site / p["path"]).read_bytes(), data)

    def test_classic_esp32_offsets(self):
        args = flasher_args("esp32", boot="0x1000", app="0x30000", otadata="0x18000")
        with Build(board="feather_esp32_v2", chip="esp32", table="partitions/8mb.csv", args=args) as b:
            m = json.loads(wm.build_manifest(b.dir, "feather_esp32_v2", b.site).read_text())
            build = m["builds"][0]
            self.assertEqual(build["chipFamily"], "ESP32")
            self.assertEqual([p["offset"] for p in build["parts"]], [0x1000, 0x8000, 0x18000, 0x30000])

    def test_version_override(self):
        with Build(version=None) as b:
            m = json.loads(wm.build_manifest(b.dir, "feather_esp32s3", b.site, version="3f2a1bc").read_text())
            self.assertEqual(m["version"], "3f2a1bc")
            self.assertTrue(m["builds"][0]["parts"][0]["path"].startswith("fw/3f2a1bc/"))

    def test_missing_version(self):
        with Build(version=None) as b, self.assertRaisesRegex(wm.ManifestError, "project_description.json"):
            wm.build_manifest(b.dir, "feather_esp32s3", b.site)

    def test_wrong_board(self):
        with Build(board="esp32s3_n16r8") as b, self.assertRaisesRegex(wm.ManifestError, "built for board"):
            wm.build_manifest(b.dir, "feather_esp32s3", b.site)

    def test_refuses_debug_build(self):
        with Build(debug=True) as b:
            with self.assertRaisesRegex(wm.ManifestError, "debug build"):
                wm.build_manifest(b.dir, "feather_esp32s3", b.site)
            self.assertFalse(b.site.exists())

    def test_bad_board_name(self):
        with Build() as b, self.assertRaises(wm.ManifestError):
            wm.build_manifest(b.dir, "../x", b.site)

    def test_missing_part(self):
        args = flasher_args()
        del args["otadata"]
        with Build(args=args) as b, self.assertRaisesRegex(wm.ManifestError, "otadata"):
            wm.build_manifest(b.dir, "feather_esp32s3", b.site)

    def test_missing_part_file(self):
        with Build() as b:
            (b.dir / "dfu32.bin").unlink()
            with self.assertRaisesRegex(wm.ManifestError, "dfu32.bin not found"):
                wm.build_manifest(b.dir, "feather_esp32s3", b.site)

    def test_rerun_same_build_is_fine(self):
        with Build() as b:
            wm.build_manifest(b.dir, "feather_esp32s3", b.site)
            wm.build_manifest(b.dir, "feather_esp32s3", b.site)

    def test_two_builds_in_one_version_dir_fail(self):
        # "1.0+a" and "1.0_a" share the slug 1.0_a; the second build differs.
        with Build() as b:
            wm.build_manifest(b.dir, "feather_esp32s3", b.site, version="1.0+a")
            (b.dir / "dfu32.bin").write_bytes(b"other app")
            with self.assertRaisesRegex(wm.ManifestError, "two builds share version directory 1.0_a"):
                wm.build_manifest(b.dir, "feather_esp32s3", b.site, version="1.0_a")

    def test_erase_notice(self):
        with Build() as b:
            base = b.baseline({"4mb.csv": "Updating from v0.3.x needs an erase.", "8mb.csv": None})
            m = json.loads(wm.build_manifest(b.dir, "feather_esp32s3", b.site, baseline=base).read_text())
            self.assertEqual(m["dfu32_erase_notice"], "Updating from v0.3.x needs an erase.")

    def test_no_erase_notice(self):
        with Build(table="partitions/8mb.csv") as b:
            base = b.baseline({"4mb.csv": "Needs an erase.", "8mb.csv": None})
            m = json.loads(wm.build_manifest(b.dir, "feather_esp32s3", b.site, baseline=base).read_text())
            self.assertNotIn("dfu32_erase_notice", m)

    def test_baseline_without_table(self):
        with Build() as b, self.assertRaisesRegex(wm.ManifestError, "no entry for 4mb.csv"):
            wm.build_manifest(b.dir, "feather_esp32s3", b.site, baseline=b.baseline({"8mb.csv": None}))

    def test_repo_baseline_has_no_stray_notice_type(self):
        data = json.loads((REPO / "partitions" / "baseline.json").read_text())
        for name, entry in data["tables"].items():
            self.assertTrue(entry["erase_notice"] is None or isinstance(entry["erase_notice"], str), name)

    def test_cli(self):
        with Build() as b, contextlib.redirect_stdout(io.StringIO()) as out:
            rc = wm.main([str(b.dir), "--board", "feather_esp32s3", "--site", str(b.site)])
            self.assertEqual(rc, 0)
            self.assertIn("ESP32-S3", out.getvalue())

    def test_cli_error(self):
        with Build(debug=True) as b, contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(wm.main([str(b.dir), "--board", "feather_esp32s3", "--site", str(b.site)]), 1)
            self.assertIn("debug build", err.getvalue())


class Page(unittest.TestCase):
    """web/index.html against the rest of the repo."""
    HTML = (REPO / "web" / "index.html").read_text()

    def test_boards_match_presets(self):
        # A preset missing here would be built and published but never offered.
        presets = {p["name"] for p in json.loads((REPO / "CMakePresets.json").read_text())["configurePresets"]}
        block = re.search(r"const BOARDS = \[(.*?)\];", self.HTML, re.S).group(1)
        self.assertEqual(set(re.findall(r"id: '([a-z0-9_]+)'", block)), presets)

    def test_esp_web_tools_path_matches_fetch_tool(self):
        self.assertIn(f'src="vendor/esp-web-tools-{fewt.VERSION}/install-button.js"', self.HTML)
        self.assertNotRegex(self.HTML, r"<script[^>]+src=\"https?://")


class PartitionBaseline(unittest.TestCase):
    TABLE = """nvs, data, nvs, 0x9000, 0x6000,
otadata, data, ota, 0xF000, 0x2000,
phy_init, data, phy, 0x11000, 0x1000,
runlog, data, 0x41, 0x12000, 0xE000,
ota_0, app, ota_0, 0x20000, 0x170000,
ota_1, app, ota_1, 0x190000, 0x170000,
staging, data, 0x40, 0x300000, 0xD0000,
coredump, data, coredump, 0x3D0000, 0x10000,
"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.csv = self.dir / "4mb.csv"
        self.csv.write_text(self.TABLE)
        self.base = self.dir / "baseline.json"

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, *args) -> tuple[int, str]:
        with contextlib.redirect_stdout(io.StringIO()) as out:
            rc = cp.main(list(args))
        return rc, out.getvalue()

    def test_repo_tables_match_baseline(self):
        base = json.loads((REPO / "partitions" / "baseline.json").read_text())
        for name in ("4mb.csv", "8mb.csv"):
            self.assertEqual(cp.check_baseline(REPO / "partitions" / name, base), [], name)

    def test_write_then_check(self):
        self.assertEqual(self.run_main("--write-baseline", str(self.base), f"{self.csv}:4M")[0], 0)
        data = json.loads(self.base.read_text())
        self.assertEqual(data["tables"]["4mb.csv"]["partitions"]["staging"], {"offset": "0x300000", "size": "0xD0000"})
        self.assertIsNone(data["tables"]["4mb.csv"]["erase_notice"])
        self.assertEqual(self.run_main("--baseline", str(self.base), f"{self.csv}:4M")[0], 0)

    def test_moved_partition_fails(self):
        self.run_main("--write-baseline", str(self.base), f"{self.csv}:4M")
        self.csv.write_text(self.TABLE.replace("staging, data, 0x40, 0x300000, 0xD0000",
                                               "staging, data, 0x40, 0x300000, 0xC0000"))
        rc, out = self.run_main("--baseline", str(self.base), f"{self.csv}:4M")
        self.assertEqual(rc, 1)
        self.assertIn("staging: 0x300000 size 0xC0000 differs from the baseline 0x300000 size 0xD0000", out)

    def test_unkept_partition_may_change(self):
        self.run_main("--write-baseline", str(self.base), f"{self.csv}:4M")
        self.csv.write_text(self.TABLE.replace("coredump, data, coredump, 0x3D0000, 0x10000",
                                               "coredump, data, coredump, 0x3E0000, 0x10000"))
        self.assertEqual(self.run_main("--baseline", str(self.base), f"{self.csv}:4M")[0], 0)

    def test_missing_table_fails(self):
        self.base.write_text(json.dumps({"tables": {}}))
        rc, out = self.run_main("--baseline", str(self.base), f"{self.csv}:4M")
        self.assertEqual(rc, 1)
        self.assertIn("no baseline for 4mb.csv", out)

    def test_rewrite_keeps_erase_notice(self):
        self.run_main("--write-baseline", str(self.base), f"{self.csv}:4M")
        data = json.loads(self.base.read_text())
        data["tables"]["4mb.csv"]["erase_notice"] = "Needs an erase."
        self.base.write_text(json.dumps(data))
        self.run_main("--write-baseline", str(self.base), f"{self.csv}:4M")
        self.assertEqual(json.loads(self.base.read_text())["tables"]["4mb.csv"]["erase_notice"], "Needs an erase.")


if __name__ == "__main__":
    unittest.main()
