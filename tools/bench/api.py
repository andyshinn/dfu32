# SPDX-License-Identifier: MIT
"""Minimal client for the DFU32 HTTP API (/api/v1), standard library only."""
from __future__ import annotations

import json
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Optional

_DEFAULT = object()


class ApiUnreachable(RuntimeError):
    """No HTTP response at all (WiFi off, device rebooting, wrong address)."""


class ApiTimeout(ApiUnreachable):
    """Connected, or tried to, but no response within the timeout."""


def retry_after(headers: dict, default: float = 1.0) -> float:
    """Seconds from a Retry-After header; an HTTP-date or junk falls back to `default`."""
    try:
        v = float(headers.get("Retry-After", default))
    except (TypeError, ValueError):
        return default
    return v if 0 <= v <= 60 else default


@dataclass
class Response:
    status: int
    body: Any
    text: str
    elapsed: float
    headers: dict = field(default_factory=dict)

    def get(self, key: str, default=None):
        return self.body.get(key, default) if isinstance(self.body, dict) else default

    @property
    def error(self) -> str:
        return self.get("error", "") or ""

    def short(self) -> str:
        return f"{self.status} {self.text[:300]}"


class ApiClient:
    def __init__(self, host: str, *, token: Optional[str] = None, log=None, redactor=None) -> None:
        self.host = host
        self.token = token
        self._log = log
        self._redact = redactor or (lambda s: s)
        # The DFU32 is on the LAN: never route through an HTTP(S)_PROXY from the environment.
        self._opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    @property
    def base(self) -> str:
        return f"http://{self.host}/api/v1"

    def request(self, method: str, path: str, body: Any = None, *, token: Any = _DEFAULT, timeout: float = 15,
                retry_429: bool = True) -> Response:
        tok = self.token if token is _DEFAULT else token
        data = b"" if body is None and method in ("POST", "PUT") else None
        if body is not None:
            data = body.encode() if isinstance(body, str) else json.dumps(body).encode()
        tries = 0
        while True:
            tries += 1
            req = urllib.request.Request(self.base + path, data=data, method=method)
            if data is not None:
                req.add_header("Content-Type", "application/json")
            if tok:
                req.add_header("Authorization", f"Bearer {tok}")
            t0 = time.monotonic()
            try:
                with self._opener.open(req, timeout=timeout) as r:
                    resp = self._make(r.status, r.read(), dict(r.headers), time.monotonic() - t0)
            except urllib.error.HTTPError as e:
                resp = self._make(e.code, e.read() or b"", dict(e.headers or {}), time.monotonic() - t0)
            except OSError as e:  # URLError, timeouts and connection errors are all OSErrors
                reason = getattr(e, "reason", e)
                if isinstance(e, TimeoutError) or isinstance(reason, TimeoutError):
                    self._write(f"{method} {path} -> no response within {timeout:g} s")
                    raise ApiTimeout(f"{method} {path}: no response within {timeout:g} s") from e
                self._write(f"{method} {path} -> unreachable ({reason})")
                raise ApiUnreachable(f"{method} {path}: {reason}") from e
            self._write(f"{method} {path}" + (f" {data.decode(errors='replace')}" if data else "") +
                        f" -> {resp.status} ({resp.elapsed * 1000:.0f} ms) {resp.text[:2000]}")
            if resp.status == 429 and retry_429 and tries < 8:
                time.sleep(retry_after(resp.headers) + 0.1)
                continue
            return resp

    def _make(self, status: int, raw: bytes, headers: dict, elapsed: float) -> Response:
        text = raw.decode(errors="replace")
        try:
            body = json.loads(text) if text else None
        except json.JSONDecodeError:
            body = None
        if isinstance(body, dict) and isinstance(body.get("token"), str):
            # Claim and rotate return the token: keep it out of every log file.
            if callable(getattr(self._redact, "add", None)):
                self._redact.add(body["token"])
        return Response(status, body, text, elapsed, {k.title(): v for k, v in headers.items()})

    def _write(self, line: str) -> None:
        # A check's helper thread can outlive the run (it is joined with a timeout) and land here after shutdown.
        if self._log and not self._log.closed:
            stamp = time.strftime("%H:%M:%S")
            self._log.write(f"[{stamp}] {self._redact(line)}\n")
            self._log.flush()

    # ---- conveniences ------------------------------------------------------------------

    def get(self, path: str, **kw) -> Response:
        return self.request("GET", path, **kw)

    def post(self, path: str, body: Any = None, **kw) -> Response:
        return self.request("POST", path, body, **kw)

    def put(self, path: str, body: Any = None, **kw) -> Response:
        return self.request("PUT", path, body, **kw)

    def delete(self, path: str, **kw) -> Response:
        return self.request("DELETE", path, **kw)

    def status(self, timeout: float = 3) -> Optional[dict]:
        """GET /status, or None when the device doesn't answer."""
        try:
            r = self.get("/status", token=None, timeout=timeout)
        except ApiUnreachable:
            return None
        return r.body if r.status == 200 and isinstance(r.body, dict) else None
