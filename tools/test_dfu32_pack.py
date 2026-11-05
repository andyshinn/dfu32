#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for dfu32_pack.py. Run: python3 -m unittest discover -s tools -p 'test_*.py'"""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dfu32_pack as pack  # noqa: E402
from test_stage_image import synthetic_package  # noqa: E402


def write_zip(path: Path, bin_data: bytes, dat: bytes) -> None:
    manifest = {"manifest": {"application": {"bin_file": "app.bin", "dat_file": "app.dat"}}}
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("manifest.json", json.dumps(manifest))
        z.writestr("app.bin", bin_data)
        z.writestr("app.dat", dat)


def run(*argv: str) -> tuple[int, str]:
    err = io.StringIO()
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
        rc = pack.main(list(argv))
    return rc, err.getvalue()


class Pack(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_zip_to_artifact_dir(self):
        bin_data, dat = synthetic_package(4096)
        z = self.dir / "RAK_4631_repeater-v1.18.0-81dbae5.zip"
        write_zip(z, bin_data, dat)
        out = self.dir / "site" / "rak4631"
        rc, err = run(str(z), "--board", "rak4631", "--board", "RAK_4631", "--name", "MeshCore repeater",
                      "--out", str(out))
        self.assertEqual(rc, 0, err)
        m = json.loads((out / "manifest.json").read_text())
        self.assertEqual(m["schema"], 1)
        self.assertEqual(m["version"], "RAK_4631_repeater-v1.18.0-81dbae5")
        self.assertEqual(m["boards"], ["rak4631", "RAK_4631"])
        self.assertEqual(m["name"], "MeshCore repeater")
        self.assertEqual(m["bin"], {"url": "firmware.bin", "size": 4096,
                                    "sha256": hashlib.sha256(bin_data).hexdigest()})
        self.assertEqual(m["dat"]["size"], 14)
        self.assertEqual(m["dat"]["sha256"], hashlib.sha256(dat).hexdigest())
        self.assertEqual(m["init"]["device_type"], 0x52)
        self.assertEqual(m["init"]["sd_req"], [0xB6])
        self.assertEqual(m["init"]["crc16"], dat[12] | dat[13] << 8)
        self.assertEqual((out / "firmware.bin").read_bytes(), bin_data)
        self.assertEqual((out / "firmware.dat").read_bytes(), dat)

    def test_bin_dat_with_version(self):
        bin_data, dat = synthetic_package(1024, device_type=0x0054)
        (self.dir / "a.bin").write_bytes(bin_data)
        (self.dir / "a.dat").write_bytes(dat)
        rc, err = run("--bin", str(self.dir / "a.bin"), "--dat", str(self.dir / "a.dat"), "--board", "t1000e",
                      "--version", "v1.18.0", "--out", str(self.dir / "o"))
        self.assertEqual(rc, 0, err)
        m = json.loads((self.dir / "o" / "manifest.json").read_text())
        self.assertEqual(m["init"]["device_type"], 0x54)
        self.assertEqual(m["version"], "v1.18.0")

    def test_rejects_bad_packages(self):
        bin_data, dat = synthetic_package(1024)
        bad_crc = dat[:12] + bytes([dat[12] ^ 1, dat[13]])
        (self.dir / "a.bin").write_bytes(bin_data)
        (self.dir / "bad.dat").write_bytes(bad_crc)
        rc, err = run("--bin", str(self.dir / "a.bin"), "--dat", str(self.dir / "bad.dat"), "--board", "x",
                      "--version", "v", "--out", str(self.dir / "o"))
        self.assertEqual(rc, 1)
        self.assertIn("CRC16", err)
        self.assertFalse((self.dir / "o").exists())

        (self.dir / "a.dat").write_bytes(dat)
        rc, err = run("--bin", str(self.dir / "a.bin"), "--dat", str(self.dir / "a.dat"), "--board", "rak 4631",
                      "--version", "v", "--out", str(self.dir / "o"))
        self.assertEqual(rc, 1)
        self.assertIn("board", err)

        rc, err = run("--bin", str(self.dir / "a.bin"), "--dat", str(self.dir / "a.dat"), "--board", "x",
                      "--version", "v" * 64, "--out", str(self.dir / "o"))
        self.assertEqual(rc, 1)
        self.assertIn("version", err)

    def test_manifest_matches_firmware_parser_limits(self):
        bin_data, dat = synthetic_package(1024)
        pkt = pack.si.parse_dat(dat)
        m = pack.build_manifest(bin_data, dat, pkt, boards=["rak4631"], version="v1", created="2026-11-05T00:00:00Z")
        self.assertEqual(m["created"], "2026-11-05T00:00:00Z")
        self.assertLessEqual(len(json.dumps(m)), 8 * 1024)
        self.assertEqual(len(m["bin"]["sha256"]), 64)


class Golden(unittest.TestCase):
    """test/host/fixtures/pack_manifest_golden.json is also parsed by the C++
    host tests (test_manifest.cpp), so packer and firmware agree on the format."""

    def test_golden_manifest(self):
        fixtures = Path(__file__).resolve().parent.parent / "test" / "host" / "fixtures"
        dat = (fixtures / "meshcore-v1.18.0-rak_4631.dat").read_bytes()
        m = pack.build_manifest(b"dfu32-golden-bin" * 4, dat, pack.si.parse_dat(dat), boards=["rak4631"],
                                version="v1.18.0-81dbae5", name="MeshCore repeater", created="2026-11-05T00:00:00Z")
        self.assertEqual(json.dumps(m, indent=2) + "\n", (fixtures / "pack_manifest_golden.json").read_text())


if __name__ == "__main__":
    unittest.main()
