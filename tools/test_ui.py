# SPDX-License-Identifier: MIT
"""Tests for tools/dfu32_ui.py and the embedded web UI it builds."""
import gzip
import io
import re
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dfu32_ui  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "components" / "api" / "ui" / "index.html"


class TestBuild(unittest.TestCase):
    def test_deterministic_and_round_trips(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = Path(d) / "a.gz", Path(d) / "sub" / "b.gz"
            dfu32_ui.build(UI, a)
            dfu32_ui.build(UI, b)
            self.assertEqual(a.read_bytes(), b.read_bytes())  # same bytes every build
            self.assertEqual(gzip.decompress(a.read_bytes()), UI.read_bytes())
            mtime = a.stat().st_mtime_ns
            dfu32_ui.build(UI, a)  # unchanged input: file left alone (no needless relink)
            self.assertEqual(a.stat().st_mtime_ns, mtime)

    def test_budget(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertTrue(dfu32_ui.check(UI, dfu32_ui.DEFAULT_BUDGET))
        self.assertIn("gzipped", out.getvalue())
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertFalse(dfu32_ui.check(UI, 100))
            self.assertEqual(dfu32_ui.main(["check", str(UI), "--budget", "100"]), 1)


class TestPage(unittest.TestCase):
    """Cheap guards on the page itself (the behaviour is benched in a browser)."""

    @classmethod
    def setUpClass(cls):
        cls.html = UI.read_text()
        cls.js = re.search(r"<script>(.*)</script>", cls.html, re.S).group(1)

    def test_no_external_resources(self):
        # The device may have no internet: everything is inline.
        self.assertIsNone(re.search(r'(src|href)="https?://', self.html))
        self.assertNotIn("@import", self.html)

    def test_no_html_injection_sinks(self):
        # Device and BLE-advertiser strings go in as text only.
        for sink in ("innerHTML", "outerHTML", "insertAdjacentHTML", "document.write", "eval("):
            self.assertNotIn(sink, self.js, sink)

    def test_uses_only_known_endpoints(self):
        paths = set(re.findall(r'"(/(?:status|claim|targets|jobs|runs|staging|ble|logs|config|system)[^"]*)"', self.js))
        allowed = ("/status", "/claim", "/targets", "/jobs/current/abort", "/runs", "/staging", "/ble/scan",
                   "/logs", "/config", "/system/reboot", "/system/token/rotate", "/system/ota")
        for p in paths:
            self.assertTrue(p.startswith(allowed), p)


if __name__ == "__main__":
    unittest.main()
