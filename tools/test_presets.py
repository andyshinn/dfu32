#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for check_presets.py and build_all.py. Run: python3 -m unittest discover -s tools -p 'test_*.py'"""
from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_all  # noqa: E402
import check_presets  # noqa: E402

REPO = Path(__file__).resolve().parent.parent


def preset(name: str, **over) -> dict:
    p = {"name": name, "binaryDir": f"build/{name}", "cacheVariables": {"DFU32_BOARD": name}}
    p.update(over)
    return p


class Project:
    def __init__(self, boards: list[str], presets: list[dict]):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        for b in boards:
            (self.dir / "boards" / b).mkdir(parents=True)
            (self.dir / "boards" / b / "sdkconfig.defaults").write_text(f'CONFIG_DFU32_BOARD_NAME="{b}"\n')
        (self.dir / "CMakePresets.json").write_text(json.dumps({"version": 3, "configurePresets": presets}))


class CheckPresets(unittest.TestCase):
    def errors(self, boards, presets):
        proj = Project(boards, presets)
        try:
            return check_presets.check(proj.dir)
        finally:
            proj.tmp.cleanup()

    def test_repo_presets_match_boards(self):
        self.assertEqual(check_presets.check(REPO), [])

    def test_ok(self):
        self.assertEqual(self.errors(["a", "b"], [preset("a"), preset("b")]), [])

    def test_board_without_preset(self):
        self.assertEqual(self.errors(["a", "b"], [preset("a")]), ["board 'b' has no preset in CMakePresets.json"])

    def test_preset_without_board(self):
        self.assertEqual(self.errors(["a"], [preset("a"), preset("z")]),
                         ["preset 'z' has no boards/z/sdkconfig.defaults"])

    def test_wrong_binary_dir_and_board(self):
        errs = self.errors(["a"], [preset("a", binaryDir="build", cacheVariables={"DFU32_BOARD": "b"})])
        self.assertEqual(len(errs), 2)
        self.assertIn("binaryDir must be 'build/a'", errs[0])
        self.assertIn("DFU32_BOARD must be 'a'", errs[1])

    def test_inherits_and_duplicate(self):
        errs = self.errors(["a"], [preset("a"), preset("a", inherits="base")])
        self.assertIn("duplicate preset 'a'", errs)
        self.assertIn("preset 'a' uses 'inherits', which idf.py does not support", errs)


class BuildAll(unittest.TestCase):
    def setUp(self):
        self.proj = Project(["a", "b", "c"], [preset("b"), preset("a"), preset("c")])
        self.calls: list[list[str]] = []
        self.fail: set[str] = set()

    def tearDown(self):
        self.proj.tmp.cleanup()

    def runner(self, cmd: list[str]) -> int:
        self.calls.append(cmd)
        return 1 if cmd[2] in self.fail else 0

    def run_main(self, *argv: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            rc = build_all.main(list(argv), runner=self.runner, project_dir=self.proj.dir)
        return rc, out.getvalue()

    def test_default_builds_every_preset_in_file_order(self):
        rc, _ = self.run_main()
        self.assertEqual(rc, 0)
        self.assertEqual(self.calls, [["idf.py", "--preset", n, "build"] for n in ("b", "a", "c")])

    def test_passes_args_and_substitutes_preset(self):
        rc, _ = self.run_main("--only", "a", "build", "merge-bin", "-o", "dfu32-{preset}-merged.bin")
        self.assertEqual(rc, 0)
        self.assertEqual(self.calls, [["idf.py", "--preset", "a", "build", "merge-bin", "-o", "dfu32-a-merged.bin"]])

    def test_failure_keeps_going_and_fails_overall(self):
        self.fail = {"b"}
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertEqual(len(self.calls), 3)
        self.assertIn("b: FAILED (exit 1)", out)
        self.assertIn("c: ok", out)

    def test_only_unknown(self):
        rc, out = self.run_main("--only", "nope")
        self.assertEqual(rc, 2)
        self.assertIn("no preset named nope", out)
        self.assertEqual(self.calls, [])

    def test_list_json(self):
        rc, out = self.run_main("--list", "--json")
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(out), ["b", "a", "c"])
        self.assertEqual(self.calls, [])


if __name__ == "__main__":
    unittest.main()
