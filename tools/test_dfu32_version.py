# SPDX-License-Identifier: MIT
"""Tests for tools/dfu32_version.cmake, the firmware version picked from git tags."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "dfu32_version.cmake"
ENV = {**os.environ, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
       "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.com",
       "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.com"}


@unittest.skipUnless(shutil.which("cmake") and shutil.which("git"), "needs cmake and git")
class TestVersion(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        self.git("init", "-q")
        self.commit()

    def tearDown(self):
        self._tmp.cleanup()

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo), *args], env=ENV, check=True,
                              capture_output=True, text=True).stdout.strip()

    def commit(self):
        self.git("commit", "-q", "--allow-empty", "-m", "c")

    def version(self):
        r = subprocess.run(["cmake", f"-DREPO={self.repo}", "-P", str(SCRIPT)], env=ENV,
                           check=True, capture_output=True, text=True)
        return r.stderr.strip()  # message(NOTICE) goes to stderr

    def short(self):
        return self.git("rev-parse", "--short", "HEAD")

    def test_no_tags_is_the_hash(self):
        self.assertEqual(self.version(), self.short())

    def test_release_on_its_candidates_commit(self):
        # Plain `git describe --tags` says v0.4.0-rc.11 here.
        for t in ("v0.4.0-rc.11", "v0.4.0-rc.12", "v0.5.0"):
            self.git("tag", t)
        self.assertEqual(self.version(), "v0.5.0")

    def test_release_outranks_its_own_candidate(self):
        self.git("tag", "v0.5.0-rc.3")
        self.git("tag", "v0.5.0")
        self.assertEqual(self.version(), "v0.5.0")

    def test_numbers_compare_as_numbers(self):
        self.git("tag", "v0.4.0-rc.9")
        self.git("tag", "v0.4.0-rc.10")
        self.assertEqual(self.version(), "v0.4.0-rc.10")

    def test_annotated_tag(self):
        self.git("tag", "v0.4.0-rc.2")
        self.git("tag", "-a", "-m", "v0.4.0", "v0.4.0")
        self.assertEqual(self.version(), "v0.4.0")

    def test_between_tags_keeps_the_distance_and_hash(self):
        self.git("tag", "v0.4.0-rc.12")
        self.git("tag", "v0.5.0")
        self.commit()
        self.commit()
        self.assertEqual(self.version(), f"v0.5.0-2-g{self.short()}")

    def test_dirty(self):
        (self.repo / "f").write_text("x")
        self.git("add", "f")
        self.commit()
        self.git("tag", "v0.4.0-rc.1")
        self.git("tag", "v0.4.0")
        (self.repo / "f").write_text("y")
        self.assertEqual(self.version(), "v0.4.0-dirty")

    def test_non_version_tag_is_ignored(self):
        # x-nightly sorts after every v...: it must not become the version.
        for t in ("v0.5.0", "v0.5.1-rc.1", "x-nightly"):
            self.git("tag", t)
        self.assertEqual(self.version(), "v0.5.1-rc.1")

    def test_only_non_version_tags_keeps_describe(self):
        self.git("tag", "nightly")
        self.assertEqual(self.version(), "nightly")

    def test_incremental_build_reconfigures_after_a_commit(self):
        # As in the top-level CMakeLists.txt: version first, then project().
        with tempfile.TemporaryDirectory() as src, tempfile.TemporaryDirectory() as build:
            Path(src, "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.16)\n"
                f'include("{SCRIPT.as_posix()}")\n'
                f'dfu32_git_version(V "{self.repo.as_posix()}")\n'
                "project(t NONE)\n"
                'file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${V}")\n')
            self.git("tag", "v0.5.0")
            run = dict(env=ENV, check=True, capture_output=True)
            subprocess.run(["cmake", "-S", src, "-B", build], **run)
            self.assertEqual(Path(build, "version.txt").read_text(), "v0.5.0")
            self.commit()
            subprocess.run(["cmake", "--build", build], **run)
            self.assertEqual(Path(build, "version.txt").read_text(), f"v0.5.0-1-g{self.short()}")

    def test_not_a_checkout_prints_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run(["cmake", f"-DREPO={d}", "-P", str(SCRIPT)], env={**ENV, "GIT_CEILING_DIRECTORIES": d},
                               check=True, capture_output=True, text=True)
        self.assertEqual(r.stderr.strip(), "")


if __name__ == "__main__":
    unittest.main()
