# SPDX-License-Identifier: MIT
"""Serves the firmware under test, plus broken variants, from memory.

Replaces `dfu32_pack.py` + `python3 -m http.server` for the bench run and adds
the fault cases from BENCH.md §4 without editing files by hand:

  /good/manifest.json      the packed image, as dfu32_pack.py writes it
  /badhash/manifest.json   bin.sha256 has one hex digit changed  -> HASH_MISMATCH
  /badsize/manifest.json   firmware.bin is served one byte long   -> SIZE_MISMATCH
  /missing/manifest.json   everything 404s                        -> HTTP_ERROR

Every request is recorded, so a check can tell whether DFU32 actually
downloaded firmware.bin (fresh stage) or reused the staged copy.
"""
from __future__ import annotations

import copy
import json
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import dfu32_pack  # noqa: E402
import dfu32_stage_image as si  # noqa: E402

VARIANTS = ("good", "badhash", "badsize", "missing")


def flip_hex_digit(h: str) -> str:
    return ("1" if h[0] != "1" else "2") + h[1:]


class FirmwareSite:
    def __init__(self, bin_data: bytes, dat: bytes, *, board: str, version: str, port: int = 8000,
                 bind: str = "0.0.0.0", max_app_size: int = si.MAX_APP_SIZE) -> None:
        self.pkt = si.validate(bin_data, dat, max_app_size)
        self.bin = bin_data
        self.dat = dat
        self.board = board
        self.version = version
        self.manifest = dfu32_pack.build_manifest(bin_data, dat, self.pkt, boards=[board], version=version,
                                                  name="DFU32 bench")
        bad = copy.deepcopy(self.manifest)
        bad["bin"]["sha256"] = flip_hex_digit(bad["bin"]["sha256"])
        self._manifests = {"good": self.manifest, "badhash": bad, "badsize": self.manifest}
        self.port = port
        self.bind = bind
        self.hits: list[dict] = []
        self._lock = threading.Lock()
        self._server: Optional[ThreadingHTTPServer] = None

    @classmethod
    def from_package(cls, zip_path: Optional[Path], bin_path: Optional[Path], dat_path: Optional[Path], *,
                     board: str, version: str = "", **kw) -> "FirmwareSite":
        bin_data, dat = si.load_package(zip_path, bin_path, dat_path)
        version = version or (zip_path.stem if zip_path else "bench")
        return cls(bin_data, dat, board=board, version=version[:63], **kw)

    @property
    def sha256(self) -> str:
        return self.manifest["bin"]["sha256"]

    def staging_image(self, target_id: str) -> bytes:
        """The same image as a staging-partition blob, for writing with esptool (no WiFi needed)."""
        return si.build_image(self.bin, self.dat, target_id=target_id, board=self.board, version=self.version,
                              staged_at=int(time.time()))

    # ---- content ------------------------------------------------------------------------

    def content(self, path: str) -> Optional[tuple[bytes, str]]:
        parts = [p for p in path.split("?")[0].split("/") if p]
        if len(parts) != 2 or parts[0] not in self._manifests:
            return None
        variant, name = parts
        if name == "manifest.json":
            return (json.dumps(self._manifests[variant], indent=2) + "\n").encode(), "application/json"
        if name == "firmware.dat":
            return self.dat, "application/octet-stream"
        if name == "firmware.bin":
            return (self.bin + b"\x00" if variant == "badsize" else self.bin), "application/octet-stream"
        return None

    def url(self, host_ip: str, variant: str = "good") -> str:
        return f"http://{host_ip}:{self.port}/{variant}/manifest.json"

    def count(self, variant: str, name: str = "firmware.bin") -> int:
        with self._lock:
            return sum(1 for h in self.hits if h["path"] == f"/{variant}/{name}" and h["status"] == 200)

    # ---- server -------------------------------------------------------------------------

    def start(self) -> None:
        site = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:  # noqa: N802
                found = site.content(self.path)
                status = 200 if found else 404
                body, ctype = found if found else (b"not found\n", "text/plain")
                self.send_response(status)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass  # DFU32 hangs up on purpose when Content-Length disagrees with the manifest
                with site._lock:
                    site.hits.append({"t": time.time(), "client": self.client_address[0], "path": self.path,
                                      "status": status, "bytes": len(body)})

            def log_message(self, *args) -> None:
                pass

        try:
            self._server = ThreadingHTTPServer((self.bind, self.port), Handler)
        except OSError as e:
            raise OSError(f"cannot serve firmware on port {self.port} ({e}); pick another with --site-port") from e
        self._server.daemon_threads = True
        self.port = self._server.server_address[1]  # resolves --site-port 0
        threading.Thread(target=self._server.serve_forever, name="dfu32-site", daemon=True).start()

    def stop(self) -> None:
        if self._server:
            self._server.shutdown()
            self._server.server_close()
            self._server = None


def local_ip_for(remote: str) -> str:
    """The address of this computer's interface that routes to `remote` (no packets are sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((remote, 9))
        return s.getsockname()[0]
    finally:
        s.close()
