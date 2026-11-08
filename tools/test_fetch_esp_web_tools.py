#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for fetch_esp_web_tools.py. Run: python3 -m unittest discover -s tools -p 'test_*.py'"""
from __future__ import annotations

import hashlib
import io
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_esp_web_tools as fewt  # noqa: E402


def tarball(files: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return buf.getvalue()


GOOD = {
    "package/dist/web/install-button.js": b"import('./chunk.js')",
    "package/dist/web/chunk.js": b"export {}",
    "package/dist/install-button.js": b"not the browser bundle",
    "package/dist/web/sub/nested.js": b"skipped",
    "package/LICENSE": b"Apache License",
    "package/README.md": b"readme",
}


class Install(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.site = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_copies_bundle_and_license(self):
        data = tarball(GOOD)
        out = fewt.install(self.site, data, hashlib.sha256(data).hexdigest())
        self.assertEqual(out, self.site / "vendor" / f"esp-web-tools-{fewt.VERSION}")
        self.assertEqual(sorted(p.name for p in out.iterdir()), ["LICENSE", "chunk.js", "install-button.js"])
        self.assertEqual((out / "install-button.js").read_bytes(), b"import('./chunk.js')")

    def test_replaces_stale_files(self):
        data = tarball(GOOD)
        out = fewt.vendor_dir(self.site)
        out.mkdir(parents=True)
        (out / "stale.js").write_text("old")
        fewt.install(self.site, data, hashlib.sha256(data).hexdigest())
        self.assertFalse((out / "stale.js").exists())

    def test_hash_mismatch(self):
        with self.assertRaisesRegex(fewt.FetchError, "does not match"):
            fewt.install(self.site, tarball(GOOD), "0" * 64)
        self.assertFalse((self.site / "vendor").exists())

    def test_missing_entry(self):
        data = tarball({"package/LICENSE": b"x"})
        with self.assertRaisesRegex(fewt.FetchError, "install-button.js"):
            fewt.install(self.site, data, hashlib.sha256(data).hexdigest())

    def test_pinned_hash_shape(self):
        self.assertRegex(fewt.SHA256, r"^[0-9a-f]{64}$")
        self.assertIn(fewt.VERSION, fewt.URL)


if __name__ == "__main__":
    unittest.main()
