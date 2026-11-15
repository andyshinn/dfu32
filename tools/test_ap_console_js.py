#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for web/ap-console.js, the installer page's "Enable AP mode", run in Node.

The page types `ap password "<pw>"` and `ap on` into DFU32's serial console
(SPEC.md §9.4, §9.5). These tests drive it with a fake Web Serial port, built
from Node's ReadableStream and WritableStream, that replays scripted console
output on a fake clock and records what the page writes. They need Node 20 or
later on PATH and are skipped without it.

Run: python3 -m unittest discover -s tools -p 'test_*.py'
"""
from __future__ import annotations

import json
import shutil
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
AP_CONSOLE_JS = (REPO / "web" / "ap-console.js").as_uri()
NODE = shutil.which("node")

PROMPT = "dfu32> "

# One request per test case; results come back as JSON. Console output is a
# list of chunks, each {"s": text} or {"h": hex bytes}.
#
# A session's fake device: `at` is output sent at fake times (ms after the
# port is read), `rules` answer writes: the first rule whose `write` equals
# the written text (and whose `after` time has passed) sends its `reply`
# chunks 5 ms later, or errors the stream (`disconnect`).
RUNNER = """
import * as a from %(module)s;
const fromHex = h => Uint8Array.from(h.match(/../g) || [], b => parseInt(b, 16));
const enc = new TextEncoder(), dec = new TextDecoder();
const bytes = c => c.h !== undefined ? fromHex(c.h) : enc.encode(c.s);

function fakeSession(q) {
  let t = 0, ctl;
  const pending = [], writes = [];  // pending: [time, chunk | 'disconnect']
  const readable = new ReadableStream({ start(c) { ctl = c; } });
  const writable = new WritableStream({
    write(chunk) {
      const s = dec.decode(chunk);
      writes.push({ t, s });
      const rule = (q.rules || []).find(r => r.write === s && t >= (r.after || 0) && (r.times === undefined || r.times > 0));
      if (!rule) return;
      if (rule.times !== undefined) rule.times--;
      if (rule.disconnect) pending.push([t + 5, 'disconnect']);
      for (const c of rule.reply || []) pending.push([t + 5, c]);
    },
  });
  for (const [when, c] of q.at || []) pending.push([when, c]);
  let broken = false;
  const deliver = () => {
    pending.sort((x, y) => x[0] - y[0]);
    while (pending.length && pending[0][0] <= t) {
      const [, c] = pending.shift();
      if (broken) continue;
      if (c === 'disconnect') { broken = true; ctl.error(new Error('NetworkError')); }
      else ctl.enqueue(bytes(c));
    }
  };
  const port = {
    readable, writable,
    async close() {
      if (readable.locked || writable.locked) throw new TypeError('locked');
      this.readable = this.writable = null;
    },
  };
  const tick = () => new Promise(r => setImmediate(r));
  const sleep = async ms => { t += ms; deliver(); await tick(); await tick(); };
  return { port, writes, now: () => t, sleep };
}

const reqs = JSON.parse(await new Promise(r => { let d = ''; process.stdin.on('data', c => d += c); process.stdin.on('end', () => r(d)); }));
const out = [];
for (const q of reqs) {
  try {
    let v;
    if (q.op === 'consts') v = { min: a.MIN_PW, max: a.MAX_PW };
    else if (q.op === 'quote') v = a.quoteArg(q.s);
    else if (q.op === 'validate') v = a.validateApPassword(q.pw);
    else if (q.op === 'commands') v = a.apCommands(q.pw);
    else if (q.op === 'classify') v = a.classifyReply(q.text, q.expected);
    else if (q.op === 'stream') {
      const cs = new a.ConsoleStream();
      v = q.chunks.map(c => cs.feed(bytes(c)));
    } else if (q.op === 'run') {
      const f = fakeSession(q);
      const result = await a.runCommands(f.port, q.lines, { now: f.now, sleep: f.sleep, ...(q.opts || {}) });
      let closed = true;
      try { await f.port.close(); } catch (e) { closed = false; }
      v = { result, writes: f.writes, closed, end: f.now() };
    }
    out.push({ ok: v });
  } catch (e) {
    out.push({ err: e.message });
  }
}
process.stdout.write(JSON.stringify(out));
"""


def run_js(requests: list[dict]) -> list[dict]:
    script = RUNNER % {"module": json.dumps(AP_CONSOLE_JS)}
    r = subprocess.run([NODE, "--input-type=module", "-e", script], input=json.dumps(requests),
                       capture_output=True, text=True, timeout=60)
    if r.returncode:
        raise AssertionError(f"node failed:\n{r.stderr}")
    return json.loads(r.stdout)


def one(req: dict):
    res = run_js([req])[0]
    if "err" in res:
        raise AssertionError(res["err"])
    return res["ok"]


def s(text: str) -> dict:
    return {"s": text}


def improv_frame(data: bytes, type_: int = 0x04, lf: bool = False) -> bytes:
    """An Improv Serial frame as improv/serial_frame.cpp encodes it."""
    frame = b"IMPROV" + bytes([1, type_, len(data)]) + data
    frame += bytes([sum(frame) & 0xFF])
    return frame + (b"\n" if lf else b"")


def status(security: str = "WPA2") -> str:
    return (f"  ap mode    on, {security}\n  delay      120 s\n"
            "  state      standby (station connected)\n")


def device(pw: str | None = None, *, password_reply: str | None = None, on_reply: str | None = None,
           echo: bool = True) -> list[dict]:
    """Rules for a running DFU32 in dumb mode: a prompt for each '\\n', echo, reply, status, prompt."""
    def answer(line: str, reply: str) -> dict:
        return {"write": line + "\n", "reply": [s((line if echo else "") + "\n" + reply + PROMPT)]}
    rules = [{"write": "\n", "reply": [s("\n" + PROMPT)]}]
    if pw is not None:
        rules.append(answer(f"ap password {quote(pw)}", password_reply or "AP password set.\n" + status()))
    rules.append(answer("ap on", on_reply or "AP mode on.\n" + status("WPA2" if pw else "open")))
    return rules


def quote(pw: str) -> str:
    return '"' + pw.replace("\\", "\\\\").replace('"', '\\"') + '"'


def commands_sent(writes: list[dict]) -> list[str]:
    return [w["s"] for w in writes if w["s"] != "\n"]


@unittest.skipUnless(NODE, "node is not installed")
class Commands(unittest.TestCase):
    def test_quote_arg(self):
        self.assertEqual(one({"op": "quote", "s": "a b"}), '"a b"')
        self.assertEqual(one({"op": "quote", "s": 'a"b\\c'}), '"a\\"b\\\\c"')

    def test_password_bounds(self):
        res = run_js([{"op": "validate", "pw": "x" * n} for n in (7, 8, 63, 64)])
        self.assertNotEqual(res[0]["ok"], "")
        self.assertEqual(res[1]["ok"], "")
        self.assertEqual(res[2]["ok"], "")
        self.assertNotEqual(res[3]["ok"], "")

    def test_password_alphabet(self):
        res = run_js([{"op": "validate", "pw": pw} for pw in
                      ("tab\tinside1", "café-latte", "myIMPROVpass", "", "with space \"q\" \\ ~")])
        self.assertNotEqual(res[0]["ok"], "")
        self.assertNotEqual(res[1]["ok"], "")
        self.assertIn("IMPROV", res[2]["ok"])
        self.assertEqual(res[3]["ok"], "")
        self.assertEqual(res[4]["ok"], "")

    def test_password_goes_first(self):
        self.assertEqual(one({"op": "commands", "pw": "secret pw"}), ['ap password "secret pw"', "ap on"])
        self.assertEqual(one({"op": "commands", "pw": ""}), ["ap on"])

    def test_constants_match_firmware(self):
        import re
        hpp = (REPO / "components" / "dfu32_core" / "include" / "dfu32_core" / "ap.hpp").read_text()
        want = {k: int(re.search(rf"k{k}PasswordLen = (\d+);", hpp).group(1)) for k in ("Min", "Max")}
        self.assertEqual(one({"op": "consts"}), {"min": want["Min"], "max": want["Max"]})


@unittest.skipUnless(NODE, "node is not installed")
class Stream(unittest.TestCase):
    def feed(self, *chunks: bytes) -> list[str]:
        return one({"op": "stream", "chunks": [{"h": c.hex()} for c in chunks]})

    def test_improv_frame_removed(self):
        frame = improv_frame(b"\nfake dfu32> \n")
        self.assertEqual("".join(self.feed(b"before\n" + frame + b"after\n")), "before\nafter\n")

    def test_improv_frame_split_across_chunks(self):
        data = b"x\n" + improv_frame(b"dfu32> \n", lf=True) + b"y" + PROMPT.encode()
        for cut in range(1, len(data)):
            self.assertEqual("".join(self.feed(data[:cut], data[cut:])), "x\ny" + PROMPT, cut)
        self.assertEqual("".join(self.feed(*[bytes([b]) for b in data])), "x\ny" + PROMPT)

    def test_frame_after_prompt_and_back_to_back(self):
        data = PROMPT.encode() + improv_frame(b"\x01") + improv_frame(b"dfu32> ", type_=1) + b"ok\n"
        self.assertEqual("".join(self.feed(data)), PROMPT + "ok\n")

    def test_text_like_a_frame_is_kept(self):
        # Only version 1 and a known type start a frame.
        self.assertEqual("".join(self.feed(b"IMPROV Serial\nIMPRIMPROV\x02x\n")), "IMPROV Serial\nIMPRIMPROV\x02x\n")

    def test_csi_and_cr_stripped(self):
        data = b"\x1b[5n\x1b[0;32mdfu32> \x1b[0m\r\nline\r\n\x1b[?25h"
        self.assertEqual("".join(self.feed(data)), PROMPT + "\nline\n")
        for cut in range(1, len(data)):
            self.assertEqual("".join(self.feed(data[:cut], data[cut:])), PROMPT + "\nline\n", cut)

    def test_multibyte_split(self):
        data = "ok ✓ \U0001F389\n".encode()
        out = self.feed(data[:4], data[4:5], data[5:9], data[9:])
        self.assertEqual("".join(out), "ok ✓ \U0001F389\n")


@unittest.skipUnless(NODE, "node is not installed")
class Classify(unittest.TestCase):
    def classify(self, text: str, expected: str = "AP mode on."):
        return one({"op": "classify", "text": text, "expected": expected})

    def test_cases(self):
        self.assertEqual(self.classify("ap on\nAP mode on.\n"), "ok")
        self.assertIsNone(self.classify("ap on\nAP mode on."))  # line not finished
        self.assertIsNone(self.classify("ap on\n"))
        self.assertEqual(self.classify("ap on\nUnrecognized command\n"), "old")
        self.assertEqual(self.classify("ap x\nUsage: ap [on | off]\n"), {"error": "Usage: ap [on | off]"})
        self.assertEqual(self.classify('ap password "x"\nNot saved: too short\n', "AP password set."),
                         {"error": "Not saved: too short"})
        self.assertEqual(self.classify("ap on\nAP mode is not in this build.\n"),
                         {"error": "AP mode is not in this build."})


BOOT_NOISE = [
    [0, s("ets Jun  8 2016 00:22:57\n\nrst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)\n")],
    [0, {"h": "00ff17c3"}],
    [300, s("\x1b[0;32mI (123) net: starting WiFi\x1b[0m\n")],
    [600, s("\x1b[5n")],
    [1200, s("\nType 'help' to get the list of commands.\n")],
]


@unittest.skipUnless(NODE, "node is not installed")
class Sessions(unittest.TestCase):
    PW = 'pa ss"wo\\rd'

    def run_session(self, lines: list[str], rules: list[dict], at: list | None = None, **opts) -> dict:
        return one({"op": "run", "lines": lines, "rules": rules, "at": at or [], "opts": opts})

    def test_with_password(self):
        r = self.run_session([f"ap password {quote(self.PW)}", "ap on"], device(self.PW))
        self.assertEqual(r["result"], {"ok": True, "security": "WPA2"})
        self.assertEqual(commands_sent(r["writes"]), [f"ap password {quote(self.PW)}\n", "ap on\n"])
        self.assertTrue(r["closed"])

    def test_open(self):
        r = self.run_session(["ap on"], device())
        self.assertEqual(r["result"], {"ok": True, "security": "open"})

    def test_each_line_ends_in_one_newline(self):
        r = self.run_session([f"ap password {quote(self.PW)}", "ap on"], device(self.PW))
        for w in r["writes"]:
            self.assertTrue(w["s"].endswith("\n"))
            self.assertEqual(w["s"].count("\n"), 1)
            self.assertNotIn("\r", w["s"])

    def test_boot_noise_then_prompt(self):
        # Booting: the console ignores '\n' until 2 s in, then answers with a prompt.
        # Its terminal probe (ESC [5n) is never answered.
        rules = device()
        rules[0]["after"] = 2000
        r = self.run_session(["ap on"], rules, BOOT_NOISE)
        self.assertEqual(r["result"], {"ok": True, "security": "open"})
        first = next(w for w in r["writes"] if w["s"] != "\n")
        self.assertGreaterEqual(first["t"], 2000)
        self.assertTrue(all(w["s"] in ("\n", "ap on\n") for w in r["writes"]))
        newlines = [w["t"] for w in r["writes"] if w["s"] == "\n"]
        self.assertGreaterEqual(len(newlines), 3)
        self.assertTrue(all(900 <= b - a <= 1100 for a, b in zip(newlines, newlines[1:])), newlines)  # about once a second

    def test_stale_prompt_is_not_fresh(self):
        # A prompt the board printed before resetting is followed by boot output,
        # so the page keeps sending '\n' until a prompt is the last thing it got.
        rules = device()
        rules[0]["after"] = 3000
        at = [[0, s(PROMPT + "\nets Jun  8 2016 00:22:57\nrst:0x1 (POWERON_RESET)\n")]]
        r = self.run_session(["ap on"], rules, at)
        self.assertEqual(r["result"], {"ok": True, "security": "open"})
        self.assertGreaterEqual(next(w for w in r["writes"] if w["s"] != "\n")["t"], 3000)

    def test_reply_split_across_chunks(self):
        rules = device()
        rules[1]["reply"] = [s("ap on\nAP mo"), s("de on.\n  ap mode    on, op"), s("en\n  delay      120 s\ndfu"), s("32> ")]
        r = self.run_session(["ap on"], rules)
        self.assertEqual(r["result"], {"ok": True, "security": "open"})

    def test_reply_with_improv_frame_and_colour(self):
        rules = device()
        frame = improv_frame(b"\nAP mode on.\ndfu32> ")
        rules[1]["reply"] = [s("ap on\n"), {"h": frame.hex()}, s("AP mode on.\n" + status("open") + "\x1b[0;32mdfu32> \x1b[0m")]
        r = self.run_session(["ap on"], rules)
        self.assertEqual(r["result"], {"ok": True, "security": "open"})

    def test_old_firmware(self):
        r = self.run_session([f"ap password {quote(self.PW)}", "ap on"],
                             device(self.PW, password_reply="Unrecognized command\n"))
        self.assertEqual(r["result"], {"error": "old"})
        self.assertEqual(commands_sent(r["writes"]), [f"ap password {quote(self.PW)}\n"])  # never `ap on`
        self.assertTrue(r["closed"])

    def test_refused(self):
        r = self.run_session(["ap on"], device(on_reply="Not saved: NVS is full\n"))
        self.assertEqual(r["result"], {"error": "Not saved: NVS is full"})

    def test_no_prompt_times_out(self):
        r = self.run_session(["ap on"], [], BOOT_NOISE)
        self.assertEqual(r["result"], {"error": "timeout"})
        self.assertEqual(commands_sent(r["writes"]), [])
        self.assertGreaterEqual(r["end"], 15000)
        self.assertLess(r["end"], 16000)
        self.assertTrue(15 <= len(r["writes"]) <= 16)
        self.assertTrue(r["closed"])

    def test_no_reply_times_out(self):
        rules = device()
        rules[1]["reply"] = [s("ap on\n")]
        r = self.run_session(["ap on"], rules, replyMs=3000)
        self.assertEqual(r["result"], {"error": "timeout"})

    def test_disconnect(self):
        rules = device()
        rules[1] = {"write": "ap on\n", "reply": [s("ap o")], "disconnect": True}
        r = self.run_session(["ap on"], rules)
        self.assertEqual(r["result"], {"error": "gone"})
        self.assertTrue(r["closed"])

    def test_disconnect_while_syncing(self):
        r = self.run_session(["ap on"], [{"write": "\n", "disconnect": True}])
        self.assertEqual(r["result"], {"error": "gone"})

    def test_password_never_in_result(self):
        pw = "Not saved: hunter22"
        line = f"ap password {quote(pw)}"
        ok = self.run_session([line, "ap on"], device(pw))
        self.assertEqual(ok["result"], {"ok": True, "security": "WPA2"})
        # A linenoise smart-mode echo that wraps at 80 columns puts part of the
        # password at a line start; a refusal built from it must not show it.
        rules = device(pw)
        rules[1]["reply"] = [s(PROMPT + 'ap password "\nNot saved: hunter22"\n')]
        bad = self.run_session([line, "ap on"], rules)
        self.assertIn("error", bad["result"])
        for r in (ok, bad):
            self.assertNotIn("hunter", json.dumps(r["result"]))


if __name__ == "__main__":
    unittest.main()
