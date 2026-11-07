#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for the bench runner (tools/dfu32_bench.py, tools/bench/).

Run: python3 -m unittest discover -s tools -p 'test_*.py'

The end-to-end test drives the runner against bench/fake_dfu32.py, a rough
simulator of DFU32's console and API. Set DFU32_BENCH_SIM=all to run every
check against it (slower); by default a representative subset runs.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dfu32_bench  # noqa: E402
from bench import parse  # noqa: E402
from bench.api import retry_after  # noqa: E402
from bench.console import Redactor, SerialConsole, quote_arg, split_incomplete_escape, strip_ansi  # noqa: E402
from bench.fake_dfu32 import FakeDfu32  # noqa: E402
from bench.runner import Ctx, Repeater  # noqa: E402
from bench.site import FirmwareSite, flip_hex_digit  # noqa: E402
from test_dfu32_pack import write_zip  # noqa: E402
from test_stage_image import synthetic_package  # noqa: E402


class Parsers(unittest.TestCase):
    def test_wifi(self):
        out = ("  hostname   dfu32-ab12 (http://dfu32-ab12.local/)\n  network    BenchNet\n"
               "  connected  yes, IP 192.168.1.50, RSSI -52 dBm\n  reconnects 3\n  clock      synced (SNTP)\n")
        w = parse.parse_wifi(out)
        self.assertEqual((w["hostname"], w["ssid"], w["ip"], w["rssi"], w["reconnects"]),
                         ("dfu32-ab12", "BenchNet", "192.168.1.50", -52, 3))
        self.assertTrue(w["connected"] and w["time_synced"])
        w = parse.parse_wifi("  network    (none: wifi <ssid> [password])\n"
                             "  connected  no (stopped for a transfer), last disconnect reason 201\n")
        self.assertFalse(w["connected"])
        self.assertTrue(w["stopped"])
        self.assertEqual((w["ssid"], w["last_disconnect_reason"]), ("", 201))

    def test_runs(self):
        out = ("  RUN  RESULT              TIME  ATT  ERASED  VERSION / ERROR\n"
               "   12  ABORTED_BY_REBOOT     0.0s    0  possibly  v1.18 / DFU32 rebooted during the run\n"
               "   11  SUCCEEDED            38.4s    2  yes     v1.18\n"
               "   10  FAILED                3.1s    0  no      v1.18 / TARGET_NOT_FOUND: no advert\n")
        rows = parse.parse_runs(out)
        self.assertEqual([r["seq"] for r in rows], [12, 11, 10])
        self.assertEqual(rows[0]["erased"], "possibly")
        self.assertEqual(rows[1]["attempts"], 2)
        self.assertEqual(rows[1]["seconds"], 38.4)
        self.assertEqual(rows[2]["error"], "TARGET_NOT_FOUND: no advert")

    def test_run(self):
        out = 'run #7\n{"seq":7,"result":"SUCCEEDED","attempts":[{"n":1}]}\n--- log (23 bytes) ---\nI (1) x: y\n'
        rec, n = parse.parse_run(out)
        self.assertEqual((rec["seq"], n), (7, 23))

    def test_flash_ok_with_retries(self):
        out = ("Staged image OK (413680 B, \"v1\"). Waiting up to 300 s for F4:..: run `start ota` on the repeater "
               "now. Up to 3 attempt(s).\nWiFi is off until the flash finishes (--wifi on keeps it up).\n"
               "WiFi restored.\n  throughput        12.4 kB/s\n"
               "  attempt 1         interval 8 ms, PRN 8: link lost [LINK_OR_OVERRUN, 27000 B acked]\n"
               "  attempt 2         interval 30 ms, PRN 8: OK\n"
               "FLASH OK in 61.0 s (after retries). Check the repeater with `ver`.\n"
               "Recorded as run #11 (`run 11` for details).\n")
        f = parse.parse_flash(out)
        self.assertTrue(f["ok"] and f["wifi_off"] and f["wifi_restored"])
        self.assertEqual((f["run"], f["throughput_kBps"]), (11, 12.4))
        self.assertEqual([a["kind"] for a in f["attempts"]], ["LINK_OR_OVERRUN", "NONE"])

    def test_flash_failed(self):
        out = ("FLASH FAILED after 3 attempt(s): link lost\nRecorded as run #4 (`run 4` for details and log).\n"
               "The target's app was erased. The staged image is intact; run `flash --yes` again.\n")
        f = parse.parse_flash(out)
        self.assertFalse(f["ok"])
        self.assertTrue(f["failed"] and f["erased"])
        self.assertEqual((f["run"], f["error"]), (4, "link lost"))
        self.assertEqual(parse.parse_flash("Busy: console is using BLE/staging.")["error"],
                         "Busy: console is using BLE/staging.")

    def test_job(self):
        out = ("  job        #3 flash (staged) for 't096'\n  state      DFU_RUNNING / STREAM\n"
               "  progress   41368 / 413680 B, 12400 B/s\n  target     ERASED (START reached)\n"
               "  run        #9 (`run 9`)\n  lease      job\n")
        j = parse.parse_job(out)
        self.assertEqual((j["id"], j["state"], j["phase"], j["run"], j["rate_bps"]), (3, "DFU_RUNNING", "STREAM", 9, 12400))
        self.assertTrue(j["target_erased"])

    def test_stage_check_staged_target_token(self):
        self.assertTrue(parse.parse_stage('STAGE OK: "v1" was already staged and re-verified (12 ms).')["reused"])
        s = parse.parse_stage('STAGE OK: "v1", 413680 B in 1.2 s, verified.')
        self.assertEqual((s["ok"], s["reused"], s["bytes"]), (True, False, 413680))
        s = parse.parse_stage("STAGE FAILED: HASH_MISMATCH: x\nThe previous staged image was invalidated; stage again")
        self.assertEqual((s["error"][:13], s["invalidated"]), ("HASH_MISMATCH", True))
        c = parse.parse_check('  version    v1  (x)\n  boards     a, b  -> target board "b": match\n  staged     nothing\n')
        self.assertEqual((c["version"], c["board_match"], c["staged"]), ("v1", True, "nothing"))
        st = parse.parse_staged("  bin        4096 B  sha256 " + "ab" * 32 + '\n  board      "t"  version "v"  target "x"\n'
                                "Verifying...\n  result     OK\n")
        self.assertEqual((st["sha256"], st["board"], st["verified"]), ("ab" * 32, "t", True))
        t = parse.parse_target("  id         t096\n  mac        f4:1c:36:a7:33:95 (random), bootloader after reset at x\n"
                               "  board      heltec_t096\n  manifest   http://h/m.json\n")
        self.assertEqual((t["id"], t["mac"], t["manifest"]), ("t096", "F4:1C:36:A7:33:95", "http://h/m.json"))
        self.assertEqual(parse.parse_token("New API token (the old one stops working):\nabc_DEF-123\n"), "abc_DEF-123")


class FakePort:
    def __init__(self):
        self.written = b""
        self.rts = self.dtr = False

    def read(self, n):
        time.sleep(0.01)
        return b""

    def write(self, b):
        self.written += b

    def flush(self):
        pass

    def close(self):
        pass


class Console(unittest.TestCase):
    def setUp(self):
        self.port = FakePort()
        self.tmp = tempfile.TemporaryDirectory()
        self.con = SerialConsole("fake", log_path=Path(self.tmp.name) / "s.log", opener=lambda p, b: self.port)
        self.con.open()

    def tearDown(self):
        self.con.close()
        self.tmp.cleanup()

    def test_ansi(self):
        self.assertEqual(strip_ansi("\x1b[0;32mI (1) x: y\x1b[0m"), "I (1) x: y")
        self.assertEqual(split_incomplete_escape("ab\x1b[0;3"), ("ab", "\x1b[0;3"))
        self.assertEqual(split_incomplete_escape("ab\x1b[0m"), ("ab\x1b[0m", ""))

    def test_command_output_and_echo(self):
        start = self.con.send("runs")
        self.assertEqual(self.port.written, b"runs\r")
        for chunk in (b"runs\r\n", b"\x1b[0;3", b"2mI (5) net: async log\x1b[0m\r\n", b"No runs recorded yet.\r\n", b"dfu32> "):
            self.con.feed(chunk)
        out = self.con.finish(start, 2, "runs")
        self.assertEqual(out, "I (5) net: async log\nNo runs recorded yet.")

    def test_smart_mode_redraws_are_not_a_prompt(self):
        # A REPL whose probe was answered by a real terminal redraws the line per key.
        start = self.con.send("job")
        self.con.feed(b"\rdfu32> j\x1b[0K\rdfu32> jo\x1b[0K\rdfu32> job\x1b[0K\r\n")
        self.con.feed(b"No job since boot.\r\ndfu32> ")
        self.assertEqual(self.con.finish(start, 2, "job"), "No job since boot.")

    def test_quote_arg(self):
        self.assertEqual(quote_arg("BenchNet"), "BenchNet")
        self.assertEqual(quote_arg("My Net"), '"My Net"')
        self.assertEqual(quote_arg('a"b\\c'), '"a\\"b\\\\c"')

    def test_prompt_followed_by_log_still_completes(self):
        start = self.con.send("wifi")
        self.con.feed(b"wifi\n  connected  yes\ndfu32> I (9) net: WiFi connected\n")
        self.assertEqual(self.con.finish(start, 2, "wifi"), "  connected  yes")

    def test_boot_and_token_redaction(self):
        self.con.feed(b"I (1) dfu32: DFU32 v1.2 (ESP-IDF v6.1), board profile 'feather_esp32_v2'\n")
        self.assertEqual(self.con.boots[-1]["version"], "v1.2")
        self.con.feed(b"New API token (the old one stops working):\nSECRETTOKEN123\n")
        log = (Path(self.tmp.name) / "s.log").read_text()
        self.assertNotIn("SECRETTOKEN123", log)
        self.assertIn("<redacted>", log)

    def test_download_mode_is_reported(self):
        from bench.console import DownloadMode
        mark = self.con.mark()
        self.con.feed(b"ets Jul 29 2019 12:21:46\n\nrst:0x1 (POWERON_RESET),boot:0x3 (DOWNLOAD_BOOT(UART0/UART1/SDIO_REI_REO_V2))\n"
                      b"waiting for download\n")
        with self.assertRaises(DownloadMode):
            self.con.wait_boot(len(self.con.boots), timeout=2, since=mark)

    def test_wait_line(self):
        mark = self.con.mark()
        self.con.feed(b"progress   0%\nprogress  10% (1/2)\n")
        hit = self.con.wait_line(r"progress\s+(\d+)%", 1, since=mark)
        self.assertEqual(hit[2].group(1), "0")
        self.assertIsNone(self.con.wait_line(r"nothing", 0.1, since=mark))

    def test_retry_after(self):
        self.assertEqual(retry_after({"Retry-After": "2"}), 2.0)
        self.assertEqual(retry_after({"Retry-After": "Wed, 21 Oct 2026 07:28:00 GMT"}), 1.0)
        self.assertEqual(retry_after({}), 1.0)
        self.assertEqual(retry_after({"Retry-After": "-5"}), 1.0)

    def test_redactor(self):
        r = Redactor()
        self.assertFalse(r.add("pw"))  # too short to redact safely; main() warns about it
        self.assertTrue(r.add("hunter22"))
        self.assertEqual(r("wifi net hunter22 pw"), "wifi net <redacted> pw")


class FlakyUsbPort:
    """A repeater port that vanishes right after a command is written, like the T096 after `start ota`."""

    def __init__(self, opened):
        self.opened = opened
        self.written = b""

    def reset_input_buffer(self):
        pass

    def write(self, b):
        self.written += b

    def flush(self):
        pass

    def read(self, n):
        raise OSError(6, "Device not configured")

    def close(self):
        pass


class RepeaterPort(unittest.TestCase):
    def test_port_dropping_after_write_is_not_an_error(self):
        opened = []

        def opener(port, baud):
            p = FlakyUsbPort(opened)
            opened.append(p)
            return p

        logs = []
        r = Repeater("usb", 115200, logs.append, opener=opener)
        self.assertEqual(r.command("start ota", wait=0.5), "")
        self.assertEqual(opened[0].written, b"start ota\r\n")
        self.assertIsNone(r.ser)  # dropped; reopened on the next command
        r.command("reboot", wait=0.2)
        self.assertEqual(len(opened), 2)
        self.assertTrue(any("dropped" in m for m in logs))

    def test_interrupt_sends_ctrl_c_alone(self):
        port = FakePort()
        con = SerialConsole("fake", opener=lambda p, b: port)
        con.open()
        try:
            th = __import__("threading").Timer(0.3, lambda: con.feed(b"FLASH ABORTED after 1 attempt(s): x\ndfu32> "))
            th.start()
            self.assertTrue(con.interrupt(timeout=3))
            self.assertEqual(port.written, b"\x03")
        finally:
            con.close()


class Site(unittest.TestCase):
    def test_variants_over_http(self):
        bin_data, dat = synthetic_package(4096)
        site = FirmwareSite(bin_data, dat, board="b", version="v1", port=0, bind="127.0.0.1")
        site.start()
        try:
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

            def get(path):
                with opener.open(f"http://127.0.0.1:{site.port}{path}", timeout=5) as r:
                    return r.read()

            good = json.loads(get("/good/manifest.json"))
            bad = json.loads(get("/badhash/manifest.json"))
            self.assertEqual(good["bin"]["sha256"], site.sha256)
            self.assertEqual(bad["bin"]["sha256"], flip_hex_digit(site.sha256))
            self.assertNotEqual(bad["bin"]["sha256"], good["bin"]["sha256"])
            self.assertEqual(json.loads(get("/badsize/manifest.json")), good)
            self.assertEqual(len(get("/badsize/firmware.bin")), 4097)
            self.assertEqual(get("/good/firmware.bin"), bin_data)
            with self.assertRaises(urllib.error.HTTPError) as e:
                get("/missing/manifest.json")
            self.assertEqual(e.exception.code, 404)
            self.assertEqual(site.count("good"), 1)
            self.assertEqual(len(site.staging_image("t096")), 4096 + 4096)
        finally:
            site.stop()


class Selection(unittest.TestCase):
    def test_matches(self):
        m = dfu32_bench.matches
        self.assertTrue(m("2.5", "2.5c"))
        self.assertTrue(m("3", "3.10"))
        self.assertTrue(m("3.1", "3.1"))
        self.assertFalse(m("3.1", "3.10"))
        self.assertFalse(m("3.1", "3.14"))

    def test_minimum_and_order(self):
        args = dfu32_bench.build_parser().parse_args(["--minimum"])
        self.assertEqual([t.id for t in dfu32_bench.select(args)], ["3.1", "3.2", "3.5", "5.1"])
        args = dfu32_bench.build_parser().parse_args(["--sections", "3"])
        ids = [t.id for t in dfu32_bench.select(args)]
        self.assertLess(ids.index("3.5"), ids.index("3.3"))  # scan after the LoRa happy path
        self.assertLess(ids.index("3.9"), ids.index("3.10"))


# A cross-section that keeps CI quick; DFU32_BENCH_SIM=all runs everything (about 2.5 minutes).
SUBSET = "1.3,2.3,2.5b,2.5d,3.2,3.6,3.9,3.11,3.12,4.1,5.1"


class EndToEnd(unittest.TestCase):
    """The runner against the simulator: catches runner bugs, not firmware bugs."""

    def test_against_fake_dfu32(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        d = Path(tmp.name)
        bin_data, dat = synthetic_package(4096)
        z = d / "Heltec_T096_repeater-v1.18.0.zip"
        write_zip(z, bin_data, dat)
        fake = FakeDfu32()
        port = fake.start_http()
        self.addCleanup(fake.stop)

        def pause(ctx, msg):
            ctx.con.note("ACTION (sim): " + msg)
            if "Reboot it" in msg:
                fake.ota_mode.clear()
            elif "OFF" in msg:
                fake.ap_on = fake.wifi_up = False
                for ms in (1000, 2000, 4000, 8000, 16000, 30000, 30000):
                    fake.log("net", f"WiFi disconnected (reason 201); retrying in {ms} ms", "W")
            elif "ON" in msg:
                fake.ap_on = fake.wifi_up = True
                fake.log("net", "WiFi connected, IP 127.0.0.1, http://x.local/")

        only = SUBSET if os.environ.get("DFU32_BENCH_SIM") != "all" else "1,2,3,4,5"
        argv = ["--port", "sim", "--zip", str(z), "--board", "heltec_t096", "--mac", fake.mac,
                "--device-ip", f"127.0.0.1:{port}", "--host-ip", "127.0.0.1", "--site-port", "0",
                "--compare-runs", "1", "--start-ota-timeout", "60", "--out", str(d / "out"), "--only", only]
        real_console = dfu32_bench.SerialConsole
        with mock.patch.object(dfu32_bench, "SerialConsole",
                               lambda *a, **kw: real_console(*a, opener=fake.opener, **kw)), \
                mock.patch.object(Ctx, "start_ota", lambda self, **kw: fake.start_ota()), \
                mock.patch.object(Ctx, "pause", pause), \
                mock.patch.object(Ctx, "countdown", lambda self, s, w: None), \
                mock.patch.object(Ctx, "resolve", staticmethod(lambda name, timeout=8: None)), \
                contextlib.redirect_stdout(io.StringIO()) as out:
            rc = dfu32_bench.main(argv)
        report = json.loads((d / "out" / "report.json").read_text())
        bad = {r["id"]: r["notes"][-3:] for r in report["results"] if r["status"] not in ("PASS", "WARN")}
        self.assertEqual(bad, {}, out.getvalue()[-3000:])
        self.assertEqual(rc, 0)
        self.assertTrue(report["meta"]["finished"])
        logs = (d / "out" / "serial.log").read_text() + (d / "out" / "http.log").read_text()
        for r in report["results"]:
            self.assertNotIn("Traceback", "\n".join(r["notes"]))
        self.assertNotIn(fake.token_hash, logs)


if __name__ == "__main__":
    unittest.main()
