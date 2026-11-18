#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for dfu32_release_manifest.py: describing a release and its binaries in dfu32-release.json.

Run: python3 -m unittest discover -s tools -p 'test_*.py'
"""
from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
import shutil
import tempfile
import unittest
from pathlib import Path

import dfu32_release_manifest as rm
from test_dfu32_web_manifest import flasher_args
from test_dfu32_web_versions import (CHIP_ESP32, CHIP_S3, TABLE_4MB, TABLE_8MB, app_image, esp_image,
                                     merged_image)

# (board, IDF target, chip id, table, CSV, flash size)
S3 = ("feather_esp32s3", "esp32s3", CHIP_S3, TABLE_4MB, "4mb.csv", "4MB")
V2 = ("feather_esp32_v2", "esp32", CHIP_ESP32, TABLE_8MB, "8mb.csv", "8MB")
FLASH_NIBBLE = {"4MB": 0x20, "8MB": 0x30}
IDF = "v6.1"
ELF = bytes(range(32))


def bootloader(chip_id: int, flash: str) -> bytes:
    boot = bytearray(esp_image(chip_id, [b"\x11" * 5000, b"\x22" * 700]))
    boot[3] = FLASH_NIBBLE[flash]  # flash size in the high nibble, as esptool writes it
    return bytes(boot)


def app(chip_id: int, version: str, board: str, project: str = "dfu32") -> bytes:
    """An app image whose esp_app_desc_t also names ESP-IDF and the ELF's SHA-256."""
    a = bytearray(app_image(chip_id, version, board, project))
    a[rm.APP_DESC_OFFSET + 112:rm.APP_DESC_OFFSET + 112 + len(IDF)] = IDF.encode()
    a[rm.APP_DESC_OFFSET + 144:rm.APP_DESC_OFFSET + 176] = ELF
    return bytes(a)


def baseline(notice_4mb=None, notice_8mb=None) -> dict:
    def table(entries, notice):
        return {"partitions": {label: {"offset": hex(off), "size": hex(size)}
                               for label, _, _, off, size in entries if label in rm.KEPT},
                "erase_notice": notice}
    return {"tables": {"4mb.csv": table(TABLE_4MB, notice_4mb), "8mb.csv": table(TABLE_8MB, notice_8mb)}}


class Release:
    """Fake IDF build directories for some boards, and the release assets CI would attach."""

    def __init__(self, version="v0.6.0", boards=(S3, V2), **kw):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.assets = self.root / "assets"
        self.assets.mkdir()
        self.dirs = {}
        for spec in boards:
            self.add(spec, version, **kw)

    def add(self, spec, version, project="dfu32", app_board=None, debug=False):
        board, target, chip, table, csv, flash = spec
        d = self.root / "build" / board
        (d / "bootloader").mkdir(parents=True)
        (d / "partition_table").mkdir()
        merged, parts = merged_image(chip, table, app=app(chip, version, app_board or board, project),
                                     boot=bootloader(chip, flash))
        for (_, data), rel in zip(parts, ("bootloader/bootloader.bin", "partition_table/partition-table.bin",
                                          "ota_data_initial.bin", "dfu32.bin")):
            (d / rel).write_bytes(data)
        offs = [hex(off) for off, _ in parts]
        (d / "flasher_args.json").write_text(json.dumps(flasher_args(target, offs[0], offs[3], offs[2])))
        (d / "sdkconfig").write_text("\n".join([
            f'CONFIG_DFU32_BOARD_NAME="{board}"', f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/{csv}"',
            f'CONFIG_ESPTOOLPY_FLASHSIZE="{flash}"',
            "CONFIG_DFU32_DEBUG_ENDPOINTS=y" if debug else "# CONFIG_DFU32_DEBUG_ENDPOINTS is not set"]) + "\n")
        (d / f"dfu32-{board}-merged.bin").write_bytes(merged)
        # What release-assets attaches: the merged image, and the app renamed.
        shutil.copy(d / f"dfu32-{board}-merged.bin", self.assets)
        shutil.copy(d / "dfu32.bin", self.assets / f"dfu32-{board}.bin")
        self.dirs[board] = d
        self.parts = getattr(self, "parts", {})
        self.parts[board] = parts

    def baseline_file(self, data: dict) -> Path:
        path = self.root / "baseline.json"
        path.write_text(json.dumps(data))
        return path

    def build(self, **kw):
        return rm.build(self.dirs, **kw)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.tmp.cleanup()


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Build(unittest.TestCase):
    def test_describes_every_binary(self):
        with Release() as r:
            m = r.build(tag="v0.6.0", commit="abc123", built_at="2026-11-18T10:00:00Z")
            self.assertEqual({k: m[k] for k in ("schema", "project", "source", "version", "tag", "commit",
                                                "built_at", "idf_version")},
                             {"schema": 1, "project": "dfu32", "source": "build", "version": "v0.6.0",
                              "tag": "v0.6.0", "commit": "abc123", "built_at": "2026-11-18T10:00:00Z",
                              "idf_version": IDF})
            self.assertEqual(list(m["boards"]), ["feather_esp32_v2", "feather_esp32s3"])
            b = m["boards"]["feather_esp32_v2"]
            self.assertEqual((b["chip_family"], b["flash_size"], b["partition_table"], b["erase_notice"]),
                             ("ESP32", "8MB", "8mb.csv", None))
            self.assertEqual(b["kept"], {"nvs": [0x9000, 0xF000], "runlog": [0x1B000, 0x10000],
                                         "staging": [0x630000, 0x100000]})
            self.assertEqual(b["elf_sha256"], ELF.hex())
            parts = r.parts["feather_esp32_v2"]
            self.assertEqual(b["parts"], [{"role": role, "offset": off, "size": len(data), "sha256": sha(data)}
                                          for role, (off, data) in zip(rm.ROLES, parts)])
            self.assertEqual([p["offset"] for p in b["parts"]], [0x1000, 0x8000, 0x18000, 0x30000])
            merged = (r.assets / "dfu32-feather_esp32_v2-merged.bin").read_bytes()
            self.assertEqual(b["merged"], {"asset": "dfu32-feather_esp32_v2-merged.bin", "size": len(merged),
                                           "sha256": sha(merged)})
            self.assertEqual(b["app"], {"asset": "dfu32-feather_esp32_v2.bin", "size": len(parts[3][1]),
                                        "sha256": sha(parts[3][1])})

    def test_without_a_tag(self):
        # Pull requests and main: the version is whatever the app reports.
        with Release(version="v0.6.0-3-gabc1234") as r:
            m = r.build()
            self.assertEqual((m["version"], m["tag"], m["commit"], m["built_at"]), ("v0.6.0-3-gabc1234", None, None, None))

    def test_the_tag_must_be_the_version(self):
        with Release(version="v0.6.0-dirty") as r, self.assertRaisesRegex(rm.ReleaseError, "not the tag v0.6.0"):
            r.build(tag="v0.6.0")

    def test_erase_notice_from_the_baseline(self):
        with Release() as r:
            path = r.root / "bl.json"
            path.write_text(json.dumps({"tables": {"4mb.csv": {"partitions": {}, "erase_notice": "Moved nvs."},
                                                   "8mb.csv": {"partitions": {}, "erase_notice": None}}}))
            m = r.build(baseline=path)
            self.assertEqual(m["boards"]["feather_esp32s3"]["erase_notice"], "Moved nvs.")
            self.assertIsNone(m["boards"]["feather_esp32_v2"]["erase_notice"])

    def test_the_merged_image_must_hold_each_part(self):
        with Release(boards=(S3,)) as r:
            path = r.dirs["feather_esp32s3"] / "dfu32-feather_esp32s3-merged.bin"
            data = bytearray(path.read_bytes())
            data[0x8000] ^= 0xFF
            path.write_bytes(bytes(data))
            with self.assertRaisesRegex(rm.ReleaseError, "doesn't hold partition-table.bin at 0x8000"):
                r.build()

    def test_the_merged_image_can_be_named(self):
        with Release(boards=(S3,)) as r:
            path = r.dirs["feather_esp32s3"] / "dfu32-feather_esp32s3-merged.bin"
            moved = r.root / "elsewhere.bin"
            path.rename(moved)
            with self.assertRaisesRegex(rm.ReleaseError, "not found"):
                r.build()
            self.assertIn("feather_esp32s3", r.build(merged={"feather_esp32s3": moved})["boards"])

    def test_refusals(self):
        with Release(boards=()) as r:
            r.add(S3, "v0.6.0", debug=True)
            with self.assertRaisesRegex(rm.ReleaseError, "fault-injection"):
                r.build()
        with Release(boards=()) as r:
            r.add(S3, "v0.6.0", app_board="esp32s3_n16r8")
            with self.assertRaisesRegex(rm.ReleaseError, "'dfu32' for 'esp32s3_n16r8'"):
                r.build()
        with Release(boards=()) as r:
            r.add(S3, "v0.6.0", project="other")
            with self.assertRaisesRegex(rm.ReleaseError, "the app is 'other'"):
                r.build()
        with Release(boards=()) as r:
            r.add(S3, "v0.6.0")
            r.add(V2, "v0.6.1")
            with self.assertRaisesRegex(rm.ReleaseError, "different versions"):
                r.build()
        with Release(boards=(S3,)) as r:
            with self.assertRaisesRegex(rm.ReleaseError, "built for board 'feather_esp32s3', not 'other'"):
                rm.build({"other": r.dirs["feather_esp32s3"]})
        with Release(boards=()) as r, self.assertRaisesRegex(rm.ReleaseError, "no boards"):
            r.build()


class Backfill(unittest.TestCase):
    def test_matches_what_build_writes(self):
        # A release published before dfu32-release.json gets the same description from its assets,
        # except what only the build knows (commit, time) and the CSV, found by layout.
        with Release() as r:
            built = r.build(tag="v0.6.0")
            back = rm.from_merged(r.assets, "v0.6.0", commit="abc123", baseline=baseline(notice_8mb="Moved."),
                                  warn=self.fail)
            self.assertEqual((back["source"], back["commit"], back["built_at"], back["version"], back["idf_version"]),
                             ("backfill", "abc123", None, "v0.6.0", IDF))
            for board, b in built["boards"].items():
                got = back["boards"][board]
                for key in ("chip_family", "flash_size", "partition_table", "kept", "elf_sha256", "merged", "app", "parts"):
                    self.assertEqual(got[key], b[key], f"{board} {key}")
            self.assertEqual(back["boards"]["feather_esp32_v2"]["erase_notice"], "Moved.")
            self.assertIsNone(back["boards"]["feather_esp32s3"]["erase_notice"])

    def test_without_a_baseline_or_app_asset(self):
        with Release(boards=(S3,)) as r:
            (r.assets / "dfu32-feather_esp32s3.bin").unlink()  # releases up to v0.4.0-rc.10 had none
            b = rm.from_merged(r.assets, "v0.6.0", warn=self.fail)["boards"]["feather_esp32s3"]
            self.assertEqual((b["partition_table"], b["erase_notice"], b["app"]), (None, None, None))

    def test_bad_boards_left_out(self):
        with Release(boards=(S3,)) as r:
            r.add(V2, "v0.6.0", project="other")
            (r.assets / "dfu32-xiao_esp32s3-merged.bin").write_bytes(b"\xFF" * 0x9000)
            (r.assets / "dfu32-feather_esp32s3.bin").write_bytes(b"not the app")
            warnings = []
            m = rm.from_merged(r.assets, "v0.6.0", warn=warnings.append)
            self.assertEqual(list(m["boards"]), ["feather_esp32s3"])
            self.assertIsNone(m["boards"]["feather_esp32s3"]["app"])
            self.assertEqual(len(warnings), 3, warnings)

    def test_version_other_than_the_tag_warns(self):
        with Release(version="v0.4.0-dirty", boards=(S3,)) as r:
            warnings = []
            self.assertEqual(rm.from_merged(r.assets, "v0.4.0", warn=warnings.append)["version"], "v0.4.0-dirty")
            self.assertIn("'v0.4.0-dirty'", warnings[0])

    def test_nothing_usable_fails(self):
        with Release(boards=()) as r, self.assertRaisesRegex(rm.ReleaseError, "no usable"):
            rm.from_merged(r.assets, "v0.6.0")


class VerifyExtract(unittest.TestCase):
    def test_extract_writes_each_part_by_hash(self):
        with Release() as r:
            m = r.build(tag="v0.6.0")
            out = r.root / "out"
            written = rm.extract(m, r.assets, out)
            self.assertEqual(len(written), 8)
            for board, parts in r.parts.items():
                for _, data in parts:
                    self.assertEqual((out / "sha256" / f"{sha(data)}.bin").read_bytes(), data)
            self.assertEqual(len(rm.extract(m, r.assets, out, ["feather_esp32s3"])), 4)

    def test_a_tampered_asset_fails(self):
        with Release(boards=(S3,)) as r:
            m = r.build()
            path = r.assets / "dfu32-feather_esp32s3-merged.bin"
            good = path.read_bytes()
            path.write_bytes(good[:-1] + bytes([good[-1] ^ 1]))
            with self.assertRaisesRegex(rm.ReleaseError, "doesn't match the manifest"):
                rm.verify(m, r.assets)
            path.write_bytes(good)
            (r.assets / "dfu32-feather_esp32s3.bin").write_bytes(b"other")
            with self.assertRaisesRegex(rm.ReleaseError, "dfu32-feather_esp32s3.bin doesn't match"):
                rm.verify(m, r.assets)

    def test_a_wrong_part_hash_fails(self):
        with Release(boards=(S3,)) as r:
            m = r.build()
            m["boards"]["feather_esp32s3"]["parts"][2]["sha256"] = "0" * 64
            with self.assertRaisesRegex(rm.ReleaseError, "the otadata at 0xF000 doesn't match"):
                rm.verify(m, r.assets)

    def test_unknown_board_or_missing_asset(self):
        with Release(boards=(S3,)) as r:
            m = r.build()
            with self.assertRaisesRegex(rm.ReleaseError, "not in the manifest: rak11200_rak19007"):
                rm.verify(m, r.assets, ["rak11200_rak19007"])
            (r.assets / "dfu32-feather_esp32s3-merged.bin").unlink()
            with self.assertRaisesRegex(rm.ReleaseError, "not found"):
                rm.verify(m, r.assets)

    def test_shape(self):
        with Release(boards=(S3,)) as r:
            good = r.build()
            rm.validate(good)

            def broken(change, message):
                m = copy.deepcopy(good)
                change(m)
                with self.assertRaisesRegex(rm.ReleaseError, message):
                    rm.validate(m)

            b = lambda m: m["boards"]["feather_esp32s3"]  # noqa: E731
            broken(lambda m: m.update(schema=2), "schema 2")
            broken(lambda m: m.update(project="other"), "not a DFU32")
            broken(lambda m: m.update(boards={}), "no boards")
            broken(lambda m: m["boards"].update({"Bad Name": b(m)}), "bad board name")
            broken(lambda m: b(m)["merged"].update(asset="x.bin"), "merged must name")
            broken(lambda m: b(m)["parts"].pop(), "exactly")
            broken(lambda m: b(m)["parts"][1].update(offset="0x8000"), "integer offset")
            broken(lambda m: b(m)["parts"].reverse(), "flash order")
            broken(lambda m: b(m)["parts"][0].update(size=0x9000), "bootloader overlaps partition-table")
            broken(lambda m: b(m)["merged"].update(size=100), "past the end")


class Cli(unittest.TestCase):
    def run_main(self, args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = rm.main([str(a) for a in args])
        return code, out.getvalue(), err.getvalue()

    def test_build_verify_extract(self):
        with Release() as r:
            manifest = r.root / "dfu32-release.json"
            code, out, _ = self.run_main(["build", "--tag", "v0.6.0", "--commit", "abc", "--out", manifest,
                                          "--baseline", r.baseline_file({"tables": {
                                              "4mb.csv": {"partitions": {}, "erase_notice": None},
                                              "8mb.csv": {"partitions": {}, "erase_notice": None}}})] +
                                         [f"{b}={d}" for b, d in r.dirs.items()])
            self.assertEqual(code, 0)
            self.assertIn("v0.6.0 (build): feather_esp32_v2 ESP32, feather_esp32s3 ESP32-S3", out)
            self.assertEqual(json.loads(manifest.read_text())["commit"], "abc")
            self.assertEqual(self.run_main(["verify", manifest, "--assets", r.assets])[0], 0)
            code, out, _ = self.run_main(["extract", manifest, "--assets", r.assets, "--out", r.root / "out",
                                          "--board", "feather_esp32s3"])
            self.assertEqual(code, 0)
            self.assertIn("4 parts in", out)

    def test_from_merged(self):
        with Release(boards=(S3,)) as r:
            manifest = r.root / "m.json"
            bl = r.baseline_file(baseline())
            code, out, _ = self.run_main(["from-merged", "--tag", "v0.6.0", "--assets", r.assets,
                                          "--baseline", bl, "--out", manifest])
            self.assertEqual(code, 0)
            self.assertIn("(backfill)", out)
            self.assertEqual(json.loads(manifest.read_text())["boards"]["feather_esp32s3"]["partition_table"], "4mb.csv")

    def test_errors(self):
        with Release(boards=(S3,)) as r:
            code, _, err = self.run_main(["build", "--out", r.root / "m.json", "feather_esp32s3"])
            self.assertEqual(code, 1)
            self.assertIn("expected BOARD=PATH", err)
            code, _, err = self.run_main(["verify", r.root / "missing.json", "--assets", r.assets])
            self.assertEqual(code, 1)
            self.assertIn("error:", err)


if __name__ == "__main__":
    unittest.main()
