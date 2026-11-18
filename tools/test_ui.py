# SPDX-License-Identifier: MIT
"""Tests for tools/dfu32_ui.py and the embedded web UI it builds."""
import gzip
import io
import json
import re
import shutil
import subprocess
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
        paths = set(re.findall(r'"(/(?:status|claim|targets|jobs|runs|staging|ble|logs|config|system|wifi)[^"]*)"', self.js))
        allowed = ("/status", "/claim", "/targets", "/jobs/current/abort", "/runs", "/staging", "/ble/scan",
                   "/logs", "/config", "/system/reboot", "/system/token/rotate", "/system/token/clear", "/system/ota",
                   "/wifi", "/wifi/scan")
        for p in paths:
            self.assertTrue(p.startswith(allowed), p)

    def test_every_option_has_help(self):
        # Options are built with opt(), which adds the "?" and its explanation;
        # a bare label would be an option without one.
        self.assertNotIn('h("label', self.js.replace('h("label.chk", el, text)', "").replace('h("label", { for: el.id }', ""))
        self.assertGreater(sum(self.js.count(f) for f in ("opt(", "field(", "knob(")), 20)

    def test_target_form(self):
        form = self.js[self.js.index("async function viewTarget"):self.js.index("function scanSection")]
        fields = re.findall(r'(?:field|knob)\("(\w+)"|opt\("Address type".*?f\.(addr_type)', form)
        order = [a or b for a, b in fields]
        self.assertEqual(order[0], "mac")  # the one required setting comes first
        # One ID (the API's board); the optional name comes after it. The API id is derived, not typed.
        self.assertEqual(order[:5], ["mac", "addr_type", "board", "manifest_url", "name"])
        self.assertNotIn('field("id"', form)
        self.assertIn("interval_ms", order)
        self.assertIn('"interval_ms"', re.search(r"const NUM = \[(.*?)\]", self.js).group(1))

    def test_multiple_targets(self):
        # Every view acts on the selected target, never just the first one.
        self.assertNotIn("targets[0]", self.js)
        self.assertNotIn("one target", self.js)
        # The selection is remembered in localStorage, and storage can throw.
        for line in self.js.splitlines():
            if "localStorage." in line:
                self.assertIn("try {", line, line)
        self.assertIn('tpath(x.id) + "/default"', self.js)  # Make default
        self.assertIn('"&target=" + encodeURIComponent(t.id)', self.js)  # upload checks the selected target
        self.assertIn("max_targets", self.js)

    @unittest.skipUnless(shutil.which("node"), "node is not installed")
    def test_new_target_id(self):
        fn = re.search(r"^function newTargetId\(.*?^\}$", self.js, re.M | re.S).group(0)
        cases = [["Attic", "", []], ["", "rak4631", []], ["", "", []], ["", "", ["target"]],
                 ["Crème Brûlée!", "", []], ["  --Roof  repeater--", "x", []], ["a" * 40, "", ["a" * 31]],
                 ["Attic", "", ["attic", "attic-2"]], ["!!!", "T1000_E", []]]
        js = fn + "\nconsole.log(JSON.stringify(%s.map((c) => newTargetId(...c))));" % json.dumps(cases)
        out = json.loads(subprocess.run(["node", "-e", js], capture_output=True, text=True, check=True).stdout)
        self.assertEqual(out, ["attic", "rak4631", "target", "target-2", "creme-brulee", "roof-repeater",
                               "a" * 29 + "-2", "attic-3", "t1000-e"])
        for x in out:
            self.assertRegex(x, r"^[a-z0-9-]{1,31}$")

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

    def test_ap_bounds_are_the_firmware_constants(self):
        # The WiFi view's access-point limits (SPEC §9.5) come from dfu32_core/ap.hpp.
        hdr = (ROOT / "components" / "dfu32_core" / "include" / "dfu32_core" / "ap.hpp").read_text()

        def const(name):
            return int(re.search(r"inline constexpr uint32_t %s = (\d+);" % name, hdr).group(1))
        want = {"delay": const("kDefaultDelayS"), "min": const("kMinDelayS"), "max": const("kMaxDelayS"),
                "idle": const("kDefaultIdleS"), "idleMax": const("kMaxIdleS"),
                "pwMin": const("kMinPasswordLen"), "pwMax": const("kMaxPasswordLen")}
        got = {k: int(v) for k, v in re.findall(r"(\w+): (\d+)", re.search(r"const kAp = \{(.*?)\}", self.js).group(1))}
        self.assertEqual(got, want)

    def test_ap_idle_limit_field(self):
        # The idle limit (SPEC §9.5) is sent as ap_idle_s; 0 (off) must survive the save,
        # so it is not read with `|| default` like the delay, and the help names the bound.
        view = self.js[self.js.index("async function viewWifi"):self.js.index("// ---- system")]
        self.assertIn("ap_idle_s: idle.value === \"\" ? kAp.idle : Number(idle.value)", view)
        self.assertIn("value: cfg.ap_idle_s, min: 0, max: kAp.idleMax", view)
        self.assertRegex(view, r'opt\("Idle limit \(s\)", "[^"]*open to anyone in range[^"]*0 for off')

    def test_wifi_join_sends_ssid_hex_for_lossy_names(self):
        # A scanned name that isn't valid UTF-8 shows sanitised and is marked; Connect then
        # sends its raw bytes as ssid_hex, never both keys (SPEC §10.1, PUT /wifi).
        view = self.js[self.js.index("async function viewWifi"):self.js.index("// ---- system")]
        self.assertIn('n.ssid_hex ? " (non-UTF-8 name)" : ""', view)
        self.assertIn("hex ? { ssid_hex: hex, password: pw.value } : { ssid: p ? p.ssid : s, password: pw.value }", view)
        # The pick is compared with what the input holds after setting it (.value drops CR/LF),
        # and an unchanged pick sends the entry's own bytes, never the input's text.
        self.assertIn("ssid.value = n.ssid; picked = [n, ssid.value];", view)
        self.assertIn("p = picked && s === picked[1] ? picked[0] : null, hex = p?.ssid_hex,", view)

    def test_refused_attempt_does_not_claim_a_rollback(self):
        # A web attempt refused when due ("busy") changed nothing (review item 8).
        view = self.js[self.js.index("async function viewWifi"):self.js.index("// ---- system")]
        self.assertIn('a.detail === "busy" ? "DFU32 was busy, so nothing was changed.', view)

    def test_status_does_not_redraw_another_view(self):
        # viewStatus()'s GET (or a Check) can finish after the user moved to another view.
        body = self.js[self.js.index("function renderStatus()"):]
        self.assertIn('if (view !== "status") return;', body[:body.index("const j = status.job")])

    def test_wifi_station_part_has_no_null_child(self):
        # replaceChildren() turns null into the text "null" (h() drops it, replaceChildren doesn't).
        body = self.js[self.js.index("function renderWifi()"):]
        call = body[body.index('$("#wsta")?.replaceChildren('):body.index("\n}")]
        self.assertNotRegex(call, r":\s*null\s*\);")

    def test_wifi_view_is_routed(self):
        self.assertIn('href="#wifi"', self.html)
        self.assertIn("wifi: viewWifi", re.search(r"const VIEWS = \{(.*?)\}", self.js).group(1))


if __name__ == "__main__":
    unittest.main()
