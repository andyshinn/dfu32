#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for dfu32_notices.py.

Run: python3 -m unittest discover -s tools -p 'test_*.py'
"""
from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dfu32_notices as dn  # noqa: E402

REPO = Path(__file__).resolve().parent.parent

LOCK = """dependencies:
  espressif/button:
    component_hash: abc
    dependencies:
    - name: espressif/cmake_utilities
      registry_url: https://components.espressif.com
      require: private
      version: '*'
    - name: idf
      require: private
      version: '>=4.0'
    source:
      type: service
    version: {button}
  espressif/cmake_utilities:
    component_hash: def
    source:
      type: service
    version: 1.3.0
  idf:
    source:
      type: idf
    version: {idf}
direct_dependencies:
- espressif/button
- idf
manifest_hash: 123
target: {target}
version: 3.0.0
"""


def lock(target: str = "esp32s3", button: str = "4.2.3", idf: str = "6.1.0") -> str:
    return LOCK.format(target=target, button=button, idf=idf)


class Repo:
    """A minimal repository: lock files, the vendored SDK and tools/notices."""

    def __init__(self, locks: dict[str, str]):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        for name, text in locks.items():
            (self.root / name).write_text(text, encoding="utf-8")
        sdk = self.root / dn.IMPROV_SDK
        sdk.mkdir(parents=True)
        (sdk / "README.md").write_text("- Version: 1.2.7 (`library.json`), copied unmodified.\n", encoding="utf-8")
        (sdk / "LICENSE").write_text("APACHE LICENSE TEXT\n", encoding="utf-8")
        self.notices = self.root / "notices"
        self.notices.mkdir()
        (self.notices / "esp-idf-6.1-COPYRIGHT.rst").write_text("IDF COPYRIGHT LIST\n", encoding="utf-8")
        (self.notices / "cJSON-LICENSE").write_text("CJSON MIT TEXT\n", encoding="utf-8")

    def render(self) -> str:
        return dn.render(self.root, dn.find_locks(self.root), self.notices, ewt_version="10.4.0",
                         ejs_version="0.7.0")

    def close(self) -> None:
        self._tmp.cleanup()


class ParseLock(unittest.TestCase):
    def test_reads_target_idf_and_own_versions(self):
        lk = dn.parse_lock(lock(button="4.2.3"))
        self.assertEqual(lk.target, "esp32s3")
        self.assertEqual(lk.idf, "6.1.0")
        # The nested `version: '*'` under button's own dependencies is not its version.
        self.assertEqual(lk.components, {"espressif/button": "4.2.3", "espressif/cmake_utilities": "1.3.0"})

    def test_real_lock_files(self):
        for path in dn.find_locks(REPO):
            lk = dn.parse_lock(path.read_text(encoding="utf-8"), path.name)
            self.assertTrue(lk.target)
            self.assertIn("h2zero/esp-nimble-cpp", lk.components)
            self.assertNotIn("idf", lk.components)

    def test_errors(self):
        with self.assertRaisesRegex(dn.NoticeError, "no version for espressif/button"):
            dn.parse_lock(lock().replace("    version: 4.2.3\n", ""))
        with self.assertRaisesRegex(dn.NoticeError, "no target"):
            dn.parse_lock(lock().replace("target: esp32s3\n", ""))
        with self.assertRaisesRegex(dn.NoticeError, "no idf entry"):
            dn.parse_lock(lock().replace("  idf:\n    source:\n      type: idf\n    version: 6.1.0\n", ""))


class Render(unittest.TestCase):
    def test_lists_everything_with_licenses(self):
        repo = Repo({"dependencies.lock.esp32s3": lock()})
        try:
            text = repo.render()
        finally:
            repo.close()
        self.assertIn("ESP-IDF 6.1.0", text)
        self.assertIn("espressif/button 4.2.3\n  License:  Apache-2.0", text)
        self.assertIn("espressif/cmake_utilities 1.3.0", text)
        self.assertIn("improv-wifi/sdk-cpp 1.2.7", text)
        self.assertIn("ESP Web Tools 10.4.0 (served from vendor/esp-web-tools-10.4.0/", text)
        self.assertIn("esptool-js 0.7.0 (served from vendor/esptool-js-0.7.0/", text)
        self.assertIn("IDF COPYRIGHT LIST", text)
        self.assertIn("APACHE LICENSE TEXT", text)
        # cJSON's text appears only when espressif/cjson is a dependency.
        self.assertNotIn("CJSON MIT TEXT", text)

    def test_versions_that_differ_by_target(self):
        repo = Repo({"dependencies.lock.esp32": lock("esp32", "4.2.3"),
                     "dependencies.lock.esp32s3": lock("esp32s3", "4.2.4")})
        try:
            text = repo.render()
        finally:
            repo.close()
        self.assertIn("espressif/button 4.2.3 (esp32), 4.2.4 (esp32s3)", text)
        self.assertIn("espressif/cmake_utilities 1.3.0\n", text)

    def test_unknown_component_fails(self):
        repo = Repo({"dependencies.lock.esp32s3": lock().replace("espressif/cmake_utilities:", "acme/thing:")})
        try:
            with self.assertRaisesRegex(dn.NoticeError, "no license entry for acme/thing"):
                repo.render()
        finally:
            repo.close()

    def test_new_idf_release_needs_its_copyright_list(self):
        repo = Repo({"dependencies.lock.esp32s3": lock(idf="6.2.0")})
        try:
            with self.assertRaisesRegex(dn.NoticeError, "esp-idf-6.2-COPYRIGHT.rst is missing"):
                repo.render()
        finally:
            repo.close()

    def test_targets_must_agree_on_idf(self):
        repo = Repo({"dependencies.lock.esp32": lock("esp32", idf="6.1.0"),
                     "dependencies.lock.esp32s3": lock("esp32s3", idf="6.1.1")})
        try:
            with self.assertRaisesRegex(dn.NoticeError, "different ESP-IDF versions"):
                repo.render()
        finally:
            repo.close()

    def test_deterministic(self):
        self.assertEqual(dn.render(REPO), dn.render(REPO))


class Committed(unittest.TestCase):
    def test_every_component_in_the_repo_is_known(self):
        for path in dn.find_locks(REPO):
            for name in dn.parse_lock(path.read_text(encoding="utf-8"), path.name).components:
                self.assertIn(name, dn.COMPONENTS, f"{path.name}: {name}")

    def test_check_mode(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "NOTICES"
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(dn.main(["--out", str(out), "--check"]), 1)  # missing
                self.assertEqual(dn.main(["--out", str(out)]), 0)
                self.assertEqual(dn.main(["--out", str(out), "--check"]), 0)
                out.write_text(out.read_text(encoding="utf-8") + "edited\n", encoding="utf-8")
                self.assertEqual(dn.main(["--out", str(out), "--check"]), 1)

    def test_committed_file_is_current(self):
        self.assertEqual((REPO / dn.OUTPUT).read_text(encoding="utf-8"), dn.render(REPO),
                         "THIRD_PARTY_NOTICES is out of date: run tools/dfu32_notices.py")


if __name__ == "__main__":
    unittest.main()
