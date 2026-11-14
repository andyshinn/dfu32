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
from bench import checks, parse  # noqa: E402
from bench.api import retry_after  # noqa: E402
from bench.console import Redactor, SerialConsole, quote_arg, split_incomplete_escape, strip_ansi  # noqa: E402
from bench.fake_dfu32 import FakeDfu32  # noqa: E402
from bench.runner import (Ctx, Repeater, crash_excerpt, mac_matches, parse_target_list,  # noqa: E402
                          pick_target_to_delete, restore_body)
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
        t = parse.parse_target("  id         attic (default target)\n  mac        (not set)\n  board      x\n"
                               "  manifest   (not set)\n  max app    default (798720 B for nRF52, none for nRF54)\n"
                               "  default    yes (2 of 3 targets; max 5): `target list` shows all\n")
        self.assertEqual((t["id"], t["mac"], t["board"]), ("attic", "", "x"))
        self.assertEqual(parse.parse_token("New API token (the old one stops working):\nabc_DEF-123\n"), "abc_DEF-123")


class Targets(unittest.TestCase):
    """Several targets, one the default: the runner's helpers and the simulator's model of them."""

    LIST = ("  ID                    MAC                BOARD             LAST OK\n"
            "  home                  F4:1C:36:A7:33:95  heltec_t096       v1.18.0-81dbae5\n"
            "* attic                 -                  -                 -\n"
            "2 of 5 targets. * = default: check, stage, probe, flash and blreset use it (`target use <id>`).\n")

    def test_parse_target_list(self):
        rows = parse_target_list(self.LIST)
        self.assertEqual([(r["id"], r["mac"], r["board"], r["last_ok"], r["default"]) for r in rows],
                         [("home", "F4:1C:36:A7:33:95", "heltec_t096", "v1.18.0-81dbae5", False),
                          ("attic", "", "", "", True)])

    def test_pick_target_to_delete(self):
        rows = [{"id": "a", "default": True}, {"id": "b"}, {"id": "t096"}, {"id": "c"}]
        self.assertEqual(pick_target_to_delete(rows, "t096"), "c")  # the last one that isn't the default
        self.assertEqual(pick_target_to_delete([{"id": "a", "default": True}, {"id": "t096"}], "t096"), "a")
        self.assertIsNone(pick_target_to_delete([{"id": "t096", "default": True}], "t096"))

    def test_restore_body(self):
        saved = {"id": "barn", "name": "Barn", "mac": "00:11:22:33:44:04", "prn": 8, "last_ok_version": "v1",
                 "last_ok_at": 12, "last_run": {"seq": 3}, "default": False}
        self.assertEqual(restore_body(saved), {"id": "barn", "name": "Barn", "mac": "00:11:22:33:44:04", "prn": 8})

    def test_mac_matches(self):
        self.assertTrue(mac_matches("f4:1c:36:a7:33:95", "F4:1C:36:A7:33:95"))
        self.assertTrue(mac_matches("F4:1C:36:A7:33:96", "F4:1C:36:A7:33:95"))  # its bootloader at MAC+1
        self.assertTrue(mac_matches("F4:1C:36:A7:33:00", "F4:1C:36:A7:33:FF"))  # the last byte wraps
        self.assertFalse(mac_matches("F4:1C:36:A7:33:94", "F4:1C:36:A7:33:95"))
        self.assertFalse(mac_matches("", "F4:1C:36:A7:33:95"))

    def call(self, fake, method, path, body=None):
        status, obj, _ = fake.handle(method, "/api/v1" + path, json.dumps(body) if body is not None else "", "")
        return status, obj

    def test_fake_api(self):
        fake = FakeDfu32(max_targets=3)
        self.assertEqual(self.call(fake, "GET", "/targets"), (200, {"targets": [], "default": None, "max_targets": 3}))
        self.assertIsNone(fake.status_obj()["default_target"])
        st, obj = self.call(fake, "PUT", "/targets/home", {"mac": "f4:1c:36:a7:33:95", "board": "heltec_t096"})
        self.assertEqual((st, obj["default"], obj["mac"]), (201, True, "F4:1C:36:A7:33:95"))  # the first is the default
        st, obj = self.call(fake, "PUT", "/targets/attic", {"name": "Attic"})
        self.assertEqual((st, obj["default"]), (201, False))
        st, obj = self.call(fake, "PUT", "/targets/shed", {"default": True})
        self.assertEqual((st, obj["default"]), (201, True))
        st, obj = self.call(fake, "PUT", "/targets/garage", {})
        self.assertEqual((st, obj["error"]), (409, "TARGET_LIMIT"))
        self.assertIn("at most 3 targets", obj["detail"])
        self.assertEqual(self.call(fake, "PUT", "/targets/home", {"board": "rak4631"})[0], 200)  # an update still works
        self.assertEqual(self.call(fake, "POST", "/targets/attic/default"), (200, {"default": "attic"}))
        self.assertEqual(self.call(fake, "POST", "/targets/nope/default")[0], 404)
        status = fake.status_obj()
        self.assertEqual(status["default_target"], "attic")
        self.assertEqual([(t["id"], t["default"]) for t in status["targets"]],
                         [("home", False), ("attic", True), ("shed", False)])
        self.assertNotIn("mac", status["targets"][0])  # /status is public
        st, obj = self.call(fake, "GET", "/targets/attic")
        self.assertEqual((st, obj["default"], obj["last_run"]), (200, True, None))
        self.assertEqual(self.call(fake, "DELETE", "/targets/attic")[0], 204)
        self.assertEqual(self.call(fake, "GET", "/targets")[1]["default"], "home")  # the first remaining one
        st, obj = self.call(fake, "POST", "/targets/attic/check")
        self.assertEqual((st, obj["detail"]), (404, "no target 'attic'"))
        self.assertEqual(self.call(fake, "DELETE", "/targets/attic")[0], 404)

    def test_fake_scan_matches_the_first_target_with_the_mac(self):
        fake = FakeDfu32()
        self.call(fake, "PUT", "/targets/other", {"mac": "00:11:22:33:44:55"})
        self.call(fake, "PUT", "/targets/boot", {"mac": "F4:1C:36:A7:33:94"})  # fake.mac is this one's MAC+1
        self.call(fake, "PUT", "/targets/t096", {"mac": fake.mac})
        fake.start_ota()
        st, obj = self.call(fake, "POST", "/ble/scan", {"duration_s": 1})
        self.assertEqual((st, obj["devices"][0]["matches_target"]), (200, "boot"))

    def test_fake_staging_upload_picks_a_target(self):
        fake = FakeDfu32()

        def upload(query):
            st, obj, _ = fake.handle("POST", "/api/v1/staging?" + query, b"\x00" * 64, "")
            return st, obj

        self.assertEqual(upload("board=rak4631&version=v1")[0], 200)  # no targets: nothing to compare with
        self.assertEqual(upload("board=rak4631&target=nope")[0], 404)
        self.call(fake, "PUT", "/targets/home", {"board": "heltec_t096"})
        self.call(fake, "PUT", "/targets/attic", {"board": "rak4631"})
        self.assertEqual(upload("board=rak4631")[0], 200)  # not the default's board, but attic's
        st, obj = upload("board=xiao")
        self.assertEqual((st, obj["error"]), (422, "BOARD_MISMATCH"))
        self.assertIn("no stored target", obj["detail"])
        st, obj = upload("board=rak4631&target=home")
        self.assertEqual((st, obj["error"]), (422, "BOARD_MISMATCH"))
        self.assertEqual(upload("board=heltec_t096&target=home")[0], 200)

    def test_fake_console(self):
        fake = FakeDfu32(max_targets=2)

        def cmd(line):
            while not fake.out.empty():
                fake.out.get()
            fake.write(line.encode() + b"\r")
            out = b""
            while not fake.out.empty():
                out += fake.out.get()
            return out.decode()

        self.assertIn("No target yet", cmd("target"))
        out = cmd("target F4:1C:36:A7:33:95")  # with none, `target <MAC>` creates the target `target`
        self.assertEqual(parse.parse_target(out)["id"], "target")
        self.assertIn("(default target)", out)
        self.assertIn("default    yes (1 of 1 target; max 2)", out)
        out = cmd("target add t096 F4:1C:36:A7:33:95 random")
        self.assertIn("Target 't096' added; it is now the default target.", out)
        self.assertEqual(fake.default_id, "t096")
        self.assertIn("Not saved: this DFU32 holds at most 2 targets", cmd("target add third"))
        rows = parse_target_list(cmd("target list"))
        self.assertEqual([(r["id"], r["default"]) for r in rows], [("target", False), ("t096", True)])
        self.assertIn("Default target: 'target'", cmd("target use target"))
        self.assertIn("No target 'nope'", cmd("target use nope"))
        cmd("target board heltec_t096")
        self.assertEqual(fake.targets["target"]["board"], "heltec_t096")  # edits the default
        self.assertEqual(parse.parse_target(cmd("target id home"))["id"], "home")
        self.assertEqual(list(fake.targets), ["home", "t096"])
        self.assertIn("The default target is now 't096'", cmd("target clear"))
        self.assertIn("No targets left", cmd("target delete t096"))


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

    def test_crash_excerpt(self):
        mark = self.con.mark()
        self.con.feed(b"I (1) dfu_ble: progress  10% (1/2)\n"
                      b"Guru Meditation Error: Core  0 panic'ed (LoadProhibited). Exception was unhandled.\n"
                      b"Backtrace: 0x400d1234:0x3ffb0000\nRebooting...\nets Jul 29 2019 12:21:46\n"
                      b"I (5) dfu32: DFU32 v1 (ESP-IDF v6.1), board profile 'x'\n"
                      b"W (9) dfu32: previous flash of F4 (\"v\") was interrupted by a reboot (PANIC)\n")
        text = "\n".join(crash_excerpt(self.con, mark))
        self.assertIn("LoadProhibited", text)
        self.assertIn("Backtrace", text)
        self.assertIn("(PANIC)", text)
        self.assertNotIn("progress", text)

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
    def test_each_command_opens_the_port_fresh(self):
        opened = []

        def opener(port, baud):
            p = FlakyUsbPort(opened)
            opened.append(p)
            return p

        r = Repeater("usb", 115200, lambda m: None, opener=opener)
        self.assertEqual(r.command("reboot", wait=0.2), "")  # the port dropping after the write isn't an error
        self.assertEqual(opened[1].written, b"reboot\r\n")  # not the handle opened at start-up
        r.command("reboot", wait=0.2)
        self.assertEqual(len(opened), 3)
        self.assertIsNone(r.ser)

    def test_resends_until_ok_while_the_repeater_reboots(self):
        import termios
        state = {"opens": 0, "writes": 0}

        class Booting(FlakyUsbPort):
            """Swallows the first two writes (still booting), answers OK to the third."""

            def write(self, b):
                super().write(b)
                state["writes"] += 1

            def read(self, n):
                time.sleep(0.005)
                return b"OK - mac: F4:1C:36:A7:33:95\r\n" if state["writes"] >= 3 else b""

        def opener(port, baud):
            state["opens"] += 1
            if 2 <= state["opens"] <= 3:  # the device node isn't back yet
                raise termios.error(2, "No such file or directory")
            return Booting([])

        r = Repeater("usb", 115200, lambda m: None, opener=opener)
        with mock.patch("bench.runner.time.sleep"):
            out = r.command("start ota", wait=0.05, expect="OK", window=10)
        self.assertIn("OK", out)
        self.assertEqual(state["writes"], 3)

    def test_stops_once_no_longer_needed(self):
        # DFU32 found the repeater after the first unanswered try: no retry may land after the flash.
        class Silent(FlakyUsbPort):
            def read(self, n):
                return b""

        ports = []

        def opener(p, b):
            ports.append(Silent([]))
            return ports[-1]

        calls = {"n": 0}

        def still_needed():
            calls["n"] += 1
            return calls["n"] == 1

        r = Repeater("usb", 115200, lambda m: None, opener=opener)
        with mock.patch("bench.runner.time.sleep"):
            r.command("start ota", wait=0.02, expect="OK", window=30, should_continue=still_needed)
        self.assertEqual(sum(p.written.count(b"start ota") for p in ports), 1)

    def test_gives_up_after_the_window(self):
        class Silent(FlakyUsbPort):
            def read(self, n):
                return b""

        r = Repeater("usb", 115200, lambda m: None, opener=lambda p, b: Silent([]))
        with mock.patch("bench.runner.time.sleep"):
            self.assertEqual(r.command("start ota", wait=0.02, expect="OK", window=0.3), "")

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

    def test_soak_is_opt_in(self):
        sel = lambda *a: [t.id for t in dfu32_bench.select(dfu32_bench.build_parser().parse_args(list(a)))]  # noqa: E731
        self.assertNotIn("6.1", sel())
        self.assertEqual(sel("--soak", "20"), ["6.1"])
        self.assertIn("6.1", sel("--soak", "20", "--sections", "3,6"))
        self.assertIn("3.5", sel("--soak", "20", "--sections", "3,6"))
        for extra in (["--minimum"], ["--only", "3.5"], ["--sections", "3"]):
            with contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit):
                dfu32_bench.main(["--port", "p", "--board", "b", "--zip", "z.zip", "--soak", "5", *extra])
            self.assertIn("leave out the soak", err.getvalue())

    def test_soak_faults_arg(self):
        parse_args = dfu32_bench.build_parser().parse_args
        self.assertEqual(parse_args([]).soak_faults, list(checks.SOAK_FAULTS))
        self.assertEqual(parse_args(["--soak-faults", "abort, reset"]).soak_faults, ["abort", "reset"])
        self.assertEqual(parse_args(["--soak-faults", "none"]).soak_faults, [])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parse_args(["--soak-faults", "abort,unplug"])


class SoakPlan(unittest.TestCase):
    def test_alternates_and_cycles_faults(self):
        plan = checks.soak_plan(7, ["reset", "abort"], 3)
        self.assertEqual([v for _, v, _ in plan], ["good", "alt", "good", "alt", "good", "alt", "good"])
        self.assertEqual([f for _, _, f in plan], ["", "", "reset", "", "", "abort", ""])
        self.assertEqual([f for _, _, f in checks.soak_plan(4, ["a", "b"], 1)], ["a", "b", "a", "b"])
        self.assertEqual({f for _, _, f in checks.soak_plan(5, [], 1)}, {""})
        self.assertEqual({f for _, _, f in checks.soak_plan(5, ["a"], 0)}, {""})


# A cross-section that keeps CI quick; DFU32_BENCH_SIM=all runs everything (about 2.5 minutes).
SUBSET = "1.3,2.3,2.5b,2.5d,3.2,3.6,3.9,3.11,4.1,5.1"


def owner_targets(fake, n, *, default="home"):
    """Targets the bench didn't make, as an owner would have them: `n` of them, `default` the default."""
    ids = ["home", "attic", "shed", "garage", "barn", "mast", "roof", "hill", "lake", "pier"][:n]
    for i, tid in enumerate(ids):
        fake.add_target({**fake.blank_target(tid), "mac": f"00:11:22:33:44:{i:02X}", "board": "rak4631"},
                        make_default=tid == default)
    return ids


def run_bench(test, fake, only, *extra):
    """dfu32_bench.main against `fake` with --only `only` (and `extra` args); returns (rc, report, stdout)."""
    tmp = tempfile.TemporaryDirectory()
    test.addCleanup(tmp.cleanup)
    d = Path(tmp.name)
    z = d / "Heltec_T096_repeater-v1.18.0.zip"
    write_zip(z, *synthetic_package(4096))
    port = fake.start_http()
    test.addCleanup(fake.stop)
    argv = ["--port", "sim", "--zip", str(z), "--board", "heltec_t096", "--mac", fake.mac,
            "--device-ip", f"127.0.0.1:{port}", "--host-ip", "127.0.0.1", "--site-port", "0",
            "--start-ota-timeout", "60", "--out", str(d / "out"), "--only", only, *extra]
    real_console = dfu32_bench.SerialConsole
    with mock.patch.object(dfu32_bench, "SerialConsole", lambda *a, **kw: real_console(*a, opener=fake.opener, **kw)), \
            mock.patch.object(Ctx, "start_ota", lambda self, **kw: fake.start_ota()), \
            mock.patch.object(Ctx, "countdown", lambda self, s, w: None), \
            mock.patch.object(Ctx, "resolve", staticmethod(lambda name, timeout=8: None)), \
            contextlib.redirect_stdout(io.StringIO()) as out:
        rc = dfu32_bench.main(argv)
    return rc, json.loads((d / "out" / "report.json").read_text()), out.getvalue()


class EndToEndTargets(unittest.TestCase):
    """The bench on a DFU32 that already holds the owner's targets."""

    def assert_passed(self, rc, report, out):
        bad = {r["id"]: r["notes"][-3:] for r in report["results"] if r["status"] not in ("PASS", "WARN")}
        self.assertEqual(bad, {}, out[-3000:])
        self.assertEqual(rc, 0)

    def snapshot(self, fake):
        return [dict(t) for t in fake.targets.values()], fake.default_id

    def assert_skipped_full(self, rc, report, out, tid):
        res = {r["id"]: r for r in report["results"]}[tid]
        self.assertEqual(res["status"], "SKIP", out[-3000:])
        self.assertIn("DFU32 is full (5 of 5 targets): free a slot, or pass --allow-delete-target", res["notes"][-1])
        self.assertEqual(rc, 0)
        self.assertNotIn("deleting target", out)
        self.assertEqual(report["meta"]["after_run"], [])

    def test_full_device_api_refuses(self):
        fake = FakeDfu32()
        owner_targets(fake, 5, default="attic")
        before = self.snapshot(fake)
        rc, report, out = run_bench(self, fake, "3.2")
        self.assert_skipped_full(rc, report, out, "3.2")
        self.assertEqual(self.snapshot(fake), before)  # nothing of the owner's was touched

    def test_full_device_api(self):
        fake = FakeDfu32()
        owner_targets(fake, 5, default="attic")
        before = self.snapshot(fake)
        rc, report, out = run_bench(self, fake, "3.2", "--allow-delete-target")
        self.assert_passed(rc, report, out)
        # One owner target made room (the last that isn't the default) and is back when the run ends,
        # in place of the bench's own target, which had to go to make room for it.
        self.assertIn("deleting target 'barn' to make room for 't096'", out)
        self.assertIn("was full: the default switch wasn't tested", "\n".join(report["results"][0]["notes"]))
        self.assertEqual(self.snapshot(fake), before)
        after = "\n".join(report["meta"]["after_run"])
        self.assertIn("deleted the bench target 't096' to make room for 'barn'", after)
        self.assertIn("put target 'barn' back with its saved settings", after)
        self.assertIn("'attic' is the default target, as before the run", after)

    def test_room_left_api(self):
        fake = FakeDfu32()
        owner_targets(fake, 2, default="attic")
        rc, report, out = run_bench(self, fake, "3.2")
        self.assert_passed(rc, report, out)
        notes = "\n".join(report["results"][0]["notes"])
        self.assertIn("409 TARGET_LIMIT", notes)
        self.assertIn("POST /targets/bench-extra-1/default -> 200", notes)
        self.assertEqual(list(fake.targets), ["home", "attic", "t096"])  # the throwaway targets are gone
        self.assertEqual(fake.default_id, "attic")  # and the owner's default is back

    def test_full_device_console_refuses(self):
        fake = FakeDfu32()
        owner_targets(fake, 5)
        before = self.snapshot(fake)
        rc, report, out = run_bench(self, fake, "2.3")
        self.assert_skipped_full(rc, report, out, "2.3")
        self.assertEqual(self.snapshot(fake), before)

    def test_full_device_console(self):
        fake = FakeDfu32()
        owner_targets(fake, 5, default="attic")
        before = self.snapshot(fake)
        rc, report, out = run_bench(self, fake, "2.3", "--allow-delete-target")
        self.assert_passed(rc, report, out)
        self.assertIn("deleting target 'barn' to make room for 't096'", out)
        self.assertIn("added target 't096' on the console; it is now the default target", out)
        # Afterwards: barn is back with the same settings, the bench target made room for it,
        # and the owner's default is the default again.
        self.assertEqual(self.snapshot(fake), before)
        after = "\n".join(report["meta"]["after_run"])
        self.assertIn("put target 'barn' back with its saved settings", after)
        self.assertIn("made 'attic' the default target again", after)

    def test_room_left_console(self):
        fake = FakeDfu32()
        owner_targets(fake, 2, default="attic")
        rc, report, out = run_bench(self, fake, "2.3")
        self.assert_passed(rc, report, out)
        self.assertEqual(list(fake.targets), ["home", "attic", "t096"])  # the bench target stays
        self.assertEqual((fake.targets["t096"]["mac"], fake.targets["t096"]["board"]), (fake.mac, "heltec_t096"))
        self.assertEqual(fake.targets["home"]["board"], "rak4631")  # the owner's targets weren't edited
        self.assertEqual(fake.default_id, "attic")  # console checks made t096 the default; attic is back
        self.assertEqual(report["meta"]["after_run"], ["made 'attic' the default target again"])

    def test_ctrl_c_still_restores(self):
        fake = FakeDfu32()
        owner_targets(fake, 5, default="attic")
        before = self.snapshot(fake)

        def ctrl_c(ctx, args=""):
            raise KeyboardInterrupt

        with mock.patch.object(checks, "stage_cmd", ctrl_c):  # 2.3 has added its target when it stages
            rc, report, out = run_bench(self, fake, "2.3", "--allow-delete-target")
        self.assertEqual((rc, report["results"][0]["status"], report["meta"]["finished"]), (1, "ABORTED", False))
        self.assertIn("deleting target 'barn' to make room for 't096'", out)
        self.assertEqual(self.snapshot(fake), before)
        self.assertIn("made 'attic' the default target again", report["meta"]["after_run"])


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
        owner_targets(fake, 2)  # the bench adds its own target next to these and leaves them alone
        port = fake.start_http()
        self.addCleanup(fake.stop)

        def pause(ctx, msg):
            ctx.con.note("ACTION (sim): " + msg)
            if "Reboot it" in msg:
                fake.ota_mode.clear()

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
        self.assertEqual(list(fake.targets), ["home", "attic", "t096"])
        self.assertEqual(fake.targets["home"]["board"], "rak4631")
        self.assertEqual(fake.default_id, "home")  # the owner's default is back after the console checks



class FakeRepeaterPort:
    """The repeater's USB console as the runner sees it: `start ota`, `ver`, `reboot`."""

    def __init__(self, fake):
        self.fake = fake
        self.reply = b""

    def reset_input_buffer(self):
        self.reply = b""

    def write(self, b):
        line = b.decode().strip()
        if line == "start ota":
            self.fake.start_ota()
            self.reply = f"start ota\r\n  -> OK - mac: {self.fake.mac}\r\n".encode()
        elif line == "ver":
            self.reply = b"v1.18.1\r\n"
        elif line == "reboot":
            self.fake.ota_mode.clear()
            self.reply = b""

    def flush(self):
        pass

    def read(self, n):
        out, self.reply = self.reply, b""
        if not out:
            time.sleep(0.01)
        return out

    def close(self):
        pass


class EndToEndUnattended(unittest.TestCase):
    """--repeater-port + --unattended: the runner sends `start ota` itself, before arming DFU32."""

    def test_unattended_with_repeater_port(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        d = Path(tmp.name)
        bin_data, dat = synthetic_package(4096)
        z = d / "Heltec_T096_repeater-v1.18.0.zip"
        write_zip(z, bin_data, dat)
        fake = FakeDfu32()
        port = fake.start_http()
        self.addCleanup(fake.stop)
        argv = ["--port", "sim", "--repeater-port", "sim-rep", "--unattended", "--zip", str(z),
                "--board", "heltec_t096", "--mac", fake.mac, "--device-ip", f"127.0.0.1:{port}",
                "--host-ip", "127.0.0.1", "--site-port", "0", "--compare-runs", "1", "--start-ota-timeout", "60",
                "--out", str(d / "out"), "--only", "1.1,3.5,3.3,3.6,3.7,3.9,4.1,5.1"]
        real_console = dfu32_bench.SerialConsole
        with mock.patch.object(dfu32_bench, "SerialConsole",
                               lambda *a, **kw: real_console(*a, opener=fake.opener, **kw)), \
                mock.patch.object(Repeater, "_pyserial", lambda self, p, b: FakeRepeaterPort(fake)), \
                mock.patch.object(Ctx, "countdown", lambda self, s, w: None), \
                mock.patch.object(Ctx, "resolve", staticmethod(lambda name, timeout=8: None)), \
                contextlib.redirect_stdout(io.StringIO()) as out:
            rc = dfu32_bench.main(argv)
        report = json.loads((d / "out" / "report.json").read_text())
        bad = {r["id"]: r["notes"][-3:] for r in report["results"] if r["status"] not in ("PASS", "WARN")}
        self.assertEqual(bad, {}, out.getvalue()[-3000:])
        self.assertEqual(rc, 0)
        notes = "\n".join(n for r in report["results"] for n in r["notes"])
        self.assertIn("LoRa) path was not tested", notes)
        log = (d / "out" / "serial.log").read_text()
        self.assertIn("before arming DFU32", log)



class EndToEndSoak(unittest.TestCase):
    """Section 6 against the simulator: two images, every fault once, each recovered."""

    def test_soak(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        d = Path(tmp.name)
        za, zb = d / "Heltec_T096_repeater-v1.18.0-81dbae5.zip", d / "Heltec_T096_repeater-v1.17.0-0a1b2c3.zip"
        write_zip(za, *synthetic_package(4096))
        write_zip(zb, *synthetic_package(4104))
        fake = FakeDfu32()
        port = fake.start_http()
        self.addCleanup(fake.stop)
        argv = ["--port", "sim", "--zip", str(za), "--soak-zip", str(zb), "--board", "heltec_t096", "--mac", fake.mac,
                "--device-ip", f"127.0.0.1:{port}", "--host-ip", "127.0.0.1", "--site-port", "0",
                "--start-ota-timeout", "60", "--out", str(d / "out"), "--soak", "6", "--soak-fault-every", "1"]
        real_console = dfu32_bench.SerialConsole
        with mock.patch.object(dfu32_bench, "SerialConsole",
                               lambda *a, **kw: real_console(*a, opener=fake.opener, **kw)), \
                mock.patch.object(Ctx, "start_ota", lambda self, **kw: fake.start_ota()), \
                mock.patch.object(Ctx, "countdown", lambda self, s, w: None), \
                mock.patch.object(Ctx, "resolve", staticmethod(lambda name, timeout=8: None)), \
                contextlib.redirect_stdout(io.StringIO()) as out:
            rc = dfu32_bench.main(argv)
        report = json.loads((d / "out" / "report.json").read_text())
        self.assertEqual([r["id"] for r in report["results"]], ["6.1"])
        res = report["results"][0]
        self.assertIn(res["status"], ("PASS", "WARN"), "\n".join(res["notes"][-8:]) + out.getvalue()[-3000:])
        self.assertEqual(rc, 0)
        rows = res["data"]["rows"]
        self.assertEqual([r["fault"] for r in rows], ["reset", "abort", "deadline", "stall", "restore", "reset"])
        self.assertEqual([r["version"] for r in rows], ["Heltec_T096_repeater-v1.18.0-81dbae5",
                                                        "Heltec_T096_repeater-v1.17.0-0a1b2c3"] * 3)
        self.assertTrue(all(r["ok"] for r in rows), rows)
        by_fault = {r["fault"]: r for r in rows}
        self.assertEqual(by_fault["abort"]["fault_result"], "FAILED ABORTED")
        self.assertTrue(by_fault["deadline"]["fault_result"].startswith("FAILED DEADLINE_EXCEEDED"))
        self.assertEqual(by_fault["stall"]["fault_result"], "ABORTED_BY_REBOOT")
        self.assertEqual(by_fault["reset"]["fault_result"], "ABORTED_BY_REBOOT")
        self.assertIn("restore watchdog rebooted", by_fault["restore"]["fault_result"])
        for f in ("reset", "abort", "deadline", "stall"):
            self.assertTrue(by_fault[f]["recovery"].endswith("SUCCEEDED"), by_fault[f])
        self.assertEqual(by_fault["restore"]["recovery"], "")
        self.assertEqual(fake.target["last_ok_version"], "Heltec_T096_repeater-v1.17.0-0a1b2c3")
        csv_text = (d / "out" / "soak.csv").read_text()
        self.assertEqual(len(csv_text.strip().splitlines()), 7)


if __name__ == "__main__":
    unittest.main()
