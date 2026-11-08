# SPDX-License-Identifier: MIT
"""Drives the DFU32 USB-serial console (the `dfu32>` REPL).

A background thread reads the port the whole time, so every byte the device
prints ends up in serial.log with a timestamp, even while the runner is busy
talking HTTP. Commands are sent as text and complete when the next `dfu32>`
prompt appears at the start of a line.

The runner never answers linenoise's terminal probe, so the REPL runs in its
"dumb" mode: no escape sequences, no line editing, plain echo. That makes the
output easy to parse.
"""
from __future__ import annotations

import codecs
import re
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

PROMPT = "dfu32> "
_PROMPT_RE = re.compile(r"\ndfu32> ")
_ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
_CTRL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")
_BOOT = re.compile(r"DFU32 (\S+) \(ESP-IDF ([^)]*)\), board profile '([^']*)'")
_TOKEN_HEADER = "New API token (the old one stops working):"


class ConsoleError(RuntimeError):
    pass


class ConsoleRebooted(ConsoleError):
    """DFU32 rebooted while a command was running."""


class DownloadMode(ConsoleError):
    """The chip reset with GPIO0 low and sits in its ROM bootloader ("waiting for download")."""


def quote_arg(arg: str) -> str:
    """Quotes an argument for esp_console's argv splitter (spaces, quotes, backslashes)."""
    if arg and not any(c in arg for c in ' "\\\''):
        return arg
    return '"' + arg.replace("\\", "\\\\").replace('"', '\\"') + '"'


def strip_ansi(text: str) -> str:
    return _ANSI.sub("", text)


def split_incomplete_escape(text: str) -> tuple[str, str]:
    """Holds back a trailing escape sequence that hasn't fully arrived yet."""
    i = text.rfind("\x1b")
    if i == -1 or _ANSI.match(text, i) or len(text) - i > 16:
        return text, ""
    return text[:i], text[i:]


class Redactor:
    """Replaces secrets (WiFi password, API tokens) before anything is written to disk."""

    def __init__(self) -> None:
        self._secrets: set[str] = set()
        self._lock = threading.Lock()

    MIN_LEN = 4  # shorter strings would mask ordinary text all over the logs

    def add(self, secret: Optional[str]) -> bool:
        """Registers a secret; False (not redacted) when it is empty or shorter than MIN_LEN."""
        if not secret or len(secret) < self.MIN_LEN:
            return False
        with self._lock:
            self._secrets.add(secret)
        return True

    def __call__(self, text: str) -> str:
        with self._lock:
            secrets = sorted(self._secrets, key=len, reverse=True)
        for s in secrets:
            text = text.replace(s, "<redacted>")
        return text


@dataclass
class Line:
    t: float  # time.time() when the line completed
    text: str


def open_pyserial(port: str, baud: int):
    try:
        import serial  # type: ignore
    except ImportError as e:  # pragma: no cover - depends on the host
        raise ConsoleError("pyserial is required: pip install pyserial (esptool installs it too)") from e
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.1
    s.write_timeout = 5
    # Opening with DTR/RTS asserted can reset the ESP32 or hold it in its ROM
    # bootloader (the auto-reset circuit). idf.py monitor clears both too.
    s.dtr = False
    s.rts = False
    s.open()
    return s


class SerialConsole:
    def __init__(self, port: str, baud: int = 115200, *, log_path=None, eol: str = "\r",
                 redactor: Optional[Redactor] = None, opener: Callable = open_pyserial,
                 echo: Optional[Callable[[str], None]] = None) -> None:
        self.port = port
        self.baud = baud
        self.eol = eol
        self.redact = redactor or Redactor()
        self.echo = echo
        self._opener = opener
        self._log = open(log_path, "a", encoding="utf-8") if log_path else None
        self._cv = threading.Condition()
        self._text = ""
        self._pending = ""
        self._partial = ""
        self._decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self._ser = None
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._token_next = False
        self._boots_at_send = 0
        self.lines: list[Line] = []
        self.boots: list[dict] = []
        self.disconnects = 0

    # ---- port lifecycle ----------------------------------------------------------------

    def open(self) -> None:
        self._ser = self._opener(self.port, self.baud)
        self._start_reader()

    def _start_reader(self) -> None:
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="dfu32-serial", daemon=True)
        self._thread.start()

    def release(self) -> None:
        """Closes the port (e.g. so esptool can use it). reacquire() reopens it."""
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        self._close_port()
        self._meta("port released")

    def reacquire(self, timeout: float = 30) -> None:
        deadline = time.monotonic() + timeout
        last: Optional[Exception] = None
        while time.monotonic() < deadline:
            try:
                self._ser = self._opener(self.port, self.baud)
                self._meta("port reopened")
                self._start_reader()
                return
            except Exception as e:  # the port can take a moment to come back after a USB reset
                last = e
                time.sleep(0.5)
        raise ConsoleError(f"cannot reopen {self.port}: {last}")

    def close(self) -> None:
        self.release()
        if self._log:
            self._log.close()
            self._log = None

    def _close_port(self) -> None:
        ser, self._ser = self._ser, None
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass

    def _run(self) -> None:
        stop = self._stop
        while not stop.is_set():
            ser = self._ser
            if ser is None:
                # Lost the port (native USB re-enumerates when the chip resets).
                try:
                    self._ser = self._opener(self.port, self.baud)
                    self._meta("port reconnected")
                except Exception:
                    time.sleep(0.5)
                continue
            try:
                data = ser.read(4096)
            except Exception as e:
                if stop.is_set():
                    break
                self.disconnects += 1
                self._meta(f"port lost ({e}); reconnecting")
                self._close_port()
                time.sleep(0.5)
                continue
            if data:
                self.feed(data)

    # ---- receive side ----------------------------------------------------------------

    def feed(self, data: bytes) -> None:
        """Adds raw bytes from the device (public for tests)."""
        text = self._pending + self._decoder.decode(data).replace("\r", "")
        text, self._pending = split_incomplete_escape(text)
        text = _CTRL.sub("", strip_ansi(text))
        if not text:
            return
        with self._cv:
            self._text += text
            *complete, self._partial = (self._partial + text).split("\n")
            for ln in complete:
                self._add_line(ln)
            self._cv.notify_all()

    def _add_line(self, text: str) -> None:
        now = time.time()
        if self._token_next:
            self.redact.add(text.strip())
            self._token_next = False
        if _TOKEN_HEADER in text:
            self._token_next = True
        self.lines.append(Line(now, text))
        m = _BOOT.search(text)
        if m:
            self.boots.append({"index": len(self.lines) - 1, "t": now, "version": m.group(1),
                               "idf": m.group(2), "board": m.group(3)})
        self._write_log(now, text)
        if self.echo:
            self.echo(self.redact(text))

    def _write_log(self, t: float, text: str) -> None:
        if self._log:
            stamp = time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t * 1000) % 1000:03d}"
            self._log.write(f"[{stamp}] {self.redact(text)}\n")
            self._log.flush()

    def _meta(self, msg: str) -> None:
        self._write_log(time.time(), f"### {msg}")

    def note(self, msg: str) -> None:
        """Writes a runner note into serial.log, so the log reads as a story."""
        self._meta(msg)

    # ---- commands ----------------------------------------------------------------------

    def _write(self, data: bytes) -> None:
        deadline = time.monotonic() + 15
        while True:
            ser = self._ser
            if ser is not None:
                try:
                    ser.write(data)
                    ser.flush()
                    return
                except Exception:
                    pass
            if time.monotonic() > deadline:
                raise ConsoleError(f"cannot write to {self.port}")
            time.sleep(0.3)

    def send(self, line: str) -> int:
        """Sends a line without waiting. Returns the position to pass to finish()."""
        with self._cv:
            start = len(self._text)
            self._boots_at_send = len(self.boots)
        self._write((line + self.eol).encode())
        return start

    def finish(self, start: int, timeout: float, line: str = "") -> str:
        """Waits for the prompt that ends the command sent at `start`; returns its output."""
        deadline = time.monotonic() + timeout
        boots = self._boots_at_send
        with self._cv:
            while True:
                seg = self._text[start:]
                # The command's echo (or, if a real terminal once answered
                # linenoise's probe, its redraws) ends with the newline from
                # Enter; the next prompt at a line start after that ends it.
                nl = seg.find("\n")
                m = _PROMPT_RE.search(seg, nl) if nl >= 0 else None
                if m:
                    out = seg[:m.start()]
                    break
                if len(self.boots) > boots:
                    raise ConsoleRebooted(f"DFU32 rebooted while running '{line}'")
                left = deadline - time.monotonic()
                if left <= 0:
                    raise ConsoleError(f"no prompt within {timeout:.0f} s after '{line}'")
                self._cv.wait(min(left, 0.5))
        lines = out.split("\n")
        while lines and not lines[0].strip():
            lines.pop(0)
        if lines and line and lines[0].strip().endswith(line.strip()):
            lines.pop(0)
        return "\n".join(lines).rstrip()

    def cmd(self, line: str, timeout: float = 30) -> str:
        return self.finish(self.send(line), timeout, line)

    def sync(self, attempts: int = 3, timeout: float = 3) -> None:
        """Makes sure the REPL is at its prompt (an empty line just reprints it)."""
        for _ in range(attempts):
            try:
                self.finish(self.send(""), timeout)
                return
            except ConsoleRebooted:
                self.wait_prompt_after_boot()
                return
            except ConsoleError:
                continue
        raise ConsoleError(f"no '{PROMPT.strip()}' prompt on {self.port}: wrong port, or the firmware isn't running")

    def ctrl_c(self) -> None:
        self._write(b"\x03")

    def interrupt(self, timeout: float = 90) -> bool:
        """Sends Ctrl+C alone and waits, sending nothing else, for the command to end at its prompt.

        DFU32's abort watcher only honours a read that holds nothing but Ctrl+C/Ctrl+D, so a
        newline sent right after it would cancel the abort.
        """
        with self._cv:
            start = len(self._text)
        self.ctrl_c()
        deadline = time.monotonic() + timeout
        with self._cv:
            while not _PROMPT_RE.search(self._text, start):
                left = deadline - time.monotonic()
                if left <= 0:
                    return False
                self._cv.wait(min(left, 0.5))
        return True

    # ---- log watching ------------------------------------------------------------------

    def mark(self) -> int:
        with self._cv:
            return len(self.lines)

    def wait_line(self, pattern, timeout: float, since: Optional[int] = None):
        """Waits for a complete line matching `pattern` at or after line index `since`.

        Returns (index, Line, match), or None on timeout.
        """
        rx = re.compile(pattern) if isinstance(pattern, str) else pattern
        deadline = time.monotonic() + timeout
        with self._cv:
            i = len(self.lines) if since is None else since
            while True:
                while i < len(self.lines):
                    m = rx.search(self.lines[i].text)
                    if m:
                        return i, self.lines[i], m
                    i += 1
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self._cv.wait(min(left, 0.5))

    def find_line(self, pattern, since: int = 0, until: Optional[int] = None):
        rx = re.compile(pattern) if isinstance(pattern, str) else pattern
        with self._cv:
            for i in range(since, len(self.lines) if until is None else until):
                m = rx.search(self.lines[i].text)
                if m:
                    return i, self.lines[i], m
        return None

    def text_since(self, since: int) -> str:
        with self._cv:
            return "\n".join(ln.text for ln in self.lines[since:])

    # ---- resets ------------------------------------------------------------------------

    def hard_reset(self) -> None:
        """Pulses EN through RTS, like esptool's hard reset (works on USB-UART and native USB)."""
        ser = self._ser
        if ser is None:
            raise ConsoleError("port not open")
        self._meta("hard reset (RTS)")
        try:
            ser.dtr = False  # GPIO0 high: boot the app, not the ROM downloader
            time.sleep(0.05)
            ser.rts = True
            time.sleep(0.2)
            ser.rts = False
        except Exception:
            pass  # native USB drops the port as the chip resets; the reader reconnects

    def wait_boot(self, boots_before: int, timeout: float = 45, since: Optional[int] = None) -> dict:
        """Waits for a boot banner after `boots_before` boots, then for the prompt.

        With `since` (a line index), raises DownloadMode as soon as the ROM
        reports "waiting for download" after that line.
        """
        deadline = time.monotonic() + timeout
        with self._cv:
            while len(self.boots) <= boots_before:
                if since is not None and any("waiting for download" in ln.text for ln in self.lines[since:]):
                    raise DownloadMode("DFU32 reset into its ROM download mode (GPIO0 was low: the serial DTR line)")
                left = deadline - time.monotonic()
                if left <= 0:
                    raise ConsoleError(f"DFU32 did not boot within {timeout:.0f} s")
                self._cv.wait(min(left, 0.5))
            boot = self.boots[-1]
        self.wait_prompt_after_boot(boot, max(10.0, deadline - time.monotonic()))
        return boot

    def wait_prompt_after_boot(self, boot: Optional[dict] = None, timeout: float = 30) -> None:
        boot = boot or (self.boots[-1] if self.boots else {"index": 0})
        hit = self.wait_line(r"console ready|layout self-check FAILED|refusing to mark", timeout, since=boot["index"])
        if hit is None:
            raise ConsoleError("DFU32 booted but the console never came up")
        if "console ready" not in hit[1].text:
            raise ConsoleError(f"DFU32 boot failed: {hit[1].text}")
        time.sleep(0.5)  # let linenoise probe the terminal and print the prompt
        self.sync(attempts=5)

    def boot_lines(self, boot: dict) -> str:
        """The log of one boot, from its banner up to the next boot (or now)."""
        with self._cv:
            nxt = [b["index"] for b in self.boots if b["index"] > boot["index"]]
            end = nxt[0] if nxt else len(self.lines)
            return "\n".join(ln.text for ln in self.lines[boot["index"]:end])
