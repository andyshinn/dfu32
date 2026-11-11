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
            self.assertEqual(gzip.decompress(a.read_bytes()), dfu32_ui.render(UI))
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


class TestInclude(unittest.TestCase):
    """/* @dfu32-include */ inlines the installer page's DFU package reader."""

    def test_staging_js_is_inlined(self):
        page = dfu32_ui.render(UI)
        staging = (ROOT / "web" / "staging.js").read_bytes()
        self.assertIn(staging.rstrip(b"\n"), page)
        self.assertNotIn(b"/* @dfu32-include", page)
        self.assertEqual(dfu32_ui.includes(UI), [ROOT / "web" / "staging.js"])

    def test_exposes_only_what_staging_js_exports(self):
        html = UI.read_text()
        names = re.search(r"window\.dfu32Staging = \{([^}]*)\}", html).group(1)
        exported = set(re.findall(r"^export (?:const|function|class|async function) (\w+)",
                                  (ROOT / "web" / "staging.js").read_text(), re.M))
        for name in (n.strip() for n in names.split(",")):
            self.assertIn(name, exported)

    def test_cmake_rebuilds_when_an_include_changes(self):
        cmake = (ROOT / "components" / "api" / "CMakeLists.txt").read_text()
        for inc in dfu32_ui.includes(UI):
            self.assertIn(inc.relative_to(ROOT).as_posix(), cmake)

    def test_missing_include_fails(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "page.html"
            src.write_text("<script>\n/* @dfu32-include web/nope.js */\n</script>\n")
            with self.assertRaises(FileNotFoundError):
                dfu32_ui.render(src)
            src.write_text("/* @dfu32-include ../../etc/passwd */\n")
            with self.assertRaises(FileNotFoundError):
                dfu32_ui.render(src)


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
                   "/logs", "/config", "/system/reboot", "/system/token/rotate", "/system/token/clear", "/system/ota")
        for p in paths:
            self.assertTrue(p.startswith(allowed), p)

    def test_every_option_has_help(self):
        # Options are built with opt(), which adds the "?" and its explanation;
        # a bare label would be an option without one.
        self.assertNotIn('h("label', self.js.replace('h("label.chk", el,', "").replace('h("label", h("span.lt"', ""))
        self.assertGreater(sum(self.js.count(f) for f in ("opt(", "field(", "knob(")), 20)

    def test_target_form(self):
        form = self.js[self.js.index("async function viewTarget"):self.js.index("function scanSection")]
        fields = re.findall(r'(?:field|knob)\("(\w+)"|opt\("Address type".*?f\.(addr_type)', form)
        order = [a or b for a, b in fields]
        self.assertEqual(order[0], "mac")  # the one required setting comes first
        # One ID (the API's board) stands in for id, name and board.
        self.assertEqual(order[:4], ["mac", "addr_type", "board", "manifest_url"])
        self.assertNotIn('field("name"', form)
        self.assertNotIn('field("id"', form)
        self.assertIn("interval_ms", order)
        self.assertIn('"interval_ms"', re.search(r"const NUM = \[(.*?)\]", self.js).group(1))

    def test_tuning_placeholders_are_the_firmware_defaults(self):
        hdr = (ROOT / "components" / "dfu32_core" / "include" / "dfu32_core" / "target_config.hpp").read_text()
        retry = (ROOT / "components" / "dfu_proto" / "include" / "dfu_proto" / "retry.hpp").read_text()

        def const(text, name):
            return int(re.search(r"inline constexpr uint16_t %s = (\d+);" % name, text).group(1))
        want = {"interval_ms": const(retry, "kDefaultIntervalMs"), "prn": const(retry, "kDefaultPrn"),
                "max_attempts": const(hdr, "kDefaultMaxAttempts"), "arm_timeout_s": const(hdr, "kDefaultArmTimeoutS"),
                "start_settle_ms": const(hdr, "kDefaultStartSettleMs"),
                "prn_timeout_ms": const(hdr, "kDefaultPrnTimeoutMs")}
        self.assertEqual((want["prn"], want["interval_ms"]), (4, 30))
        got = {k: int(v) for k, v in re.findall(r"(\w+): (\d+)", re.search(r"const kDef = \{(.*?)\}", self.js).group(1))}
        self.assertEqual(got, want)


if __name__ == "__main__":
    unittest.main()
