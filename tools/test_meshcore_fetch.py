#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for dfu32_meshcore_fetch.py, against a local fake of the GitHub releases API.

Run: python3 -m unittest discover -s tools -p 'test_*.py'
"""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import sys
import tempfile
import threading
import unittest
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dfu32_meshcore_fetch as mf  # noqa: E402
from test_dfu32_pack import write_zip  # noqa: E402
from test_stage_image import synthetic_package  # noqa: E402

REPO = "meshcore-dev/MeshCore"


class FakeGitHub:
    """Serves /repos/<repo>/releases[...] and the asset downloads (behind a redirect, like GitHub)."""

    def __init__(self, releases: list[dict], files: dict[str, bytes]) -> None:
        self.releases = releases
        self.files = files
        self.requests: list[tuple[str, dict]] = []
        self.rate_limited = False
        fake = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802
                fake.requests.append((self.path, dict(self.headers)))
                status, body, headers = fake.route(self.path)
                self.send_response(status)
                for k, v in headers.items():
                    self.send_header(k, v)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()

    def route(self, path: str):
        js = {"Content-Type": "application/json"}
        if self.rate_limited and path.startswith("/repos/"):
            return 403, b'{"message":"API rate limit exceeded"}', {**js, "X-RateLimit-Remaining": "0"}
        if path.startswith(f"/repos/{REPO}/releases/tags/"):
            tag = path.rsplit("/", 1)[1]
            rel = next((r for r in self.releases if r["tag_name"] == tag and not r.get("draft")), None)
            return (200, json.dumps(rel).encode(), js) if rel else (404, b'{"message":"Not Found"}', js)
        if path.startswith(f"/repos/{REPO}/releases?"):
            q = dict(p.split("=") for p in path.split("?", 1)[1].split("&"))
            per, page = int(q["per_page"]), int(q["page"])
            return 200, json.dumps(self.releases[(page - 1) * per:page * per]).encode(), js
        if path.startswith("/download/"):
            return 302, b"", {"Location": self.base + "/storage/" + path.rsplit("/", 1)[1]}
        if path.startswith("/storage/"):
            name = path.rsplit("/", 1)[1]
            if name in self.files:
                return 200, self.files[name], {"Content-Type": "application/octet-stream"}
        return 404, b"not found", {}

    def asset(self, name: str, data: bytes, *, digest: bool = True) -> dict:
        self.files[name] = data
        a = {"name": name, "size": len(data), "browser_download_url": f"{self.base}/download/x/{name}"}
        if digest:
            a["digest"] = "sha256:" + hashlib.sha256(data).hexdigest()
        return a


def release(tag: str, assets: list, **kw) -> dict:
    return {"tag_name": tag, "name": f"Repeater Firmware {tag.rsplit('-', 1)[1]}", "draft": False,
            "prerelease": False, "published_at": "2026-10-01T12:00:00Z",
            "html_url": f"https://github.com/{REPO}/releases/tag/{tag}", "assets": assets, **kw}


def no_proxy() -> urllib.request.OpenerDirector:
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


class Fetch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.gh = FakeGitHub([], {})
        self.addCleanup(self.gh.stop)
        self.bin, self.dat = synthetic_package(4096)
        zbuf = self.dir / "src.zip"
        write_zip(zbuf, self.bin, self.dat)
        self.zip = zbuf.read_bytes()
        gh = self.gh
        self.gh.releases = [
            release("companion-v1.19.0", [gh.asset("RAK_4631_companion_radio_ble-v1.19.0-aaaaaaa.zip", b"x")]),
            release("repeater-v1.19.0-rc1", [], prerelease=True),
            release("repeater-v1.18.0", [
                gh.asset("RAK_4631_repeater-v1.18.0-81dbae5.zip", self.zip),
                gh.asset("RAK_4631_repeater-v1.18.0-81dbae5.uf2", b"uf2"),
                gh.asset("Xiao_nrf52_repeater-v1.18.0-81dbae5.zip", self.zip, digest=False),
                gh.asset("Heltec_v3_repeater-v1.18.0-81dbae5.bin", b"esp"),
                gh.asset("Heltec_v3_repeater-v1.18.0-81dbae5-merged.bin", b"esp"),
            ]),
            release("repeater-v1.9.0", [gh.asset("RAK_4631_repeater-v1.9.0-0123456.zip", self.zip)]),
        ]
        p = mock.patch.object(mf, "_opener", no_proxy)
        p.start()
        self.addCleanup(p.stop)

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *argv: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = mf.main(["--api-url", self.gh.base, "--token", "", *argv])
        return rc, out.getvalue(), err.getvalue()

    def test_tag_to_artifact_dir(self):
        out = self.dir / "site" / "rak4631"
        rc, so, se = self.run_tool("--tag", "repeater-v1.18.0", "--env", "RAK_4631_repeater", "--board", "rak4631",
                                   "--out", str(out), "--keep-zip", str(self.dir / "zips"))
        self.assertEqual(rc, 0, se)
        m = json.loads((out / "manifest.json").read_text())
        self.assertEqual(m["version"], "v1.18.0-81dbae5")
        self.assertEqual(m["boards"], ["rak4631"])
        self.assertEqual(m["name"], "MeshCore repeater (RAK_4631_repeater)")
        self.assertEqual(m["notes"], f"https://github.com/{REPO}/releases/tag/repeater-v1.18.0")
        self.assertEqual(m["bin"]["sha256"], hashlib.sha256(self.bin).hexdigest())
        self.assertEqual((out / "firmware.dat").read_bytes(), self.dat)
        self.assertEqual((self.dir / "zips" / "RAK_4631_repeater-v1.18.0-81dbae5.zip").read_bytes(), self.zip)
        self.assertIn("RAK_4631_repeater-v1.18.0-81dbae5.zip", so)

    def test_latest_skips_other_kinds_and_prereleases(self):
        rc, so, se = self.run_tool("--latest", "repeater", "--env", "rak_4631_REPEATER", "--board", "rak4631",
                                   "--board", "RAK_4631", "--version", "custom", "--name", "Attic",
                                   "--out", str(self.dir / "o"))
        self.assertEqual(rc, 0, se)
        m = json.loads((self.dir / "o" / "manifest.json").read_text())
        self.assertEqual((m["version"], m["name"], m["boards"]), ("custom", "Attic", ["rak4631", "RAK_4631"]))
        self.assertIn("repeater-v1.18.0", so)

    def test_latest_prerelease(self):
        rel = mf.GitHub(self.gh.base).latest_release(REPO, "repeater", prerelease=True)
        self.assertEqual(rel["tag_name"], "repeater-v1.18.0")  # 1.19.0-rc1 doesn't parse as a version tag
        self.gh.releases[1]["tag_name"] = "repeater-v1.19.0"
        rel = mf.GitHub(self.gh.base).latest_release(REPO, "repeater", prerelease=True)
        self.assertEqual(rel["tag_name"], "repeater-v1.19.0")
        rel = mf.GitHub(self.gh.base).latest_release(REPO, "repeater")
        self.assertEqual(rel["tag_name"], "repeater-v1.18.0")

    def test_latest_pages_until_found(self):
        filler = [release(f"companion-v1.{i}.0", []) for i in range(mf.PER_PAGE)]
        self.gh.releases = filler + self.gh.releases
        rel = mf.GitHub(self.gh.base).latest_release(REPO, "repeater")
        self.assertEqual(rel["tag_name"], "repeater-v1.18.0")
        pages = [p for p, _ in self.gh.requests if "page=" in p]
        self.assertEqual(len(pages), 2)
        with self.assertRaisesRegex(mf.FetchError, "no published 'room-server-v"):
            mf.GitHub(self.gh.base).latest_release(REPO, "room-server")

    def test_list(self):
        rc, so, se = self.run_tool("--tag", "repeater-v1.18.0", "--list")
        self.assertEqual(rc, 0, se)
        self.assertIn("2 nRF52 DFU package(s)", so)
        self.assertIn("  RAK_4631_repeater\n", so)
        self.assertIn("  Xiao_nrf52_repeater\n", so)
        self.assertNotIn("Heltec_v3", so)

    def test_errors(self):
        out = str(self.dir / "o")
        rc, _, se = self.run_tool("--tag", "repeater-v9.9.9", "--env", "x", "--board", "b", "--out", out)
        self.assertEqual(rc, 1)
        self.assertIn("no published release tagged 'repeater-v9.9.9'", se)
        rc, _, se = self.run_tool("--tag", "repeater-v1.18.0", "--env", "RAK_4631_reapeter", "--board", "b", "--out", out)
        self.assertIn("did you mean RAK_4631_repeater", se)
        rc, _, se = self.run_tool("--tag", "repeater-v1.18.0", "--env", "Heltec_v3_repeater", "--board", "b", "--out", out)
        self.assertIn("has no DFU .zip", se)
        self.assertFalse((self.dir / "o").exists())

    def test_size_and_digest_checked(self):
        rel = self.gh.releases[2]
        rel["assets"][0]["size"] += 1
        rc, _, se = self.run_tool("--tag", "repeater-v1.18.0", "--env", "RAK_4631_repeater", "--board", "b",
                                  "--out", str(self.dir / "o"))
        self.assertEqual(rc, 1)
        self.assertIn("the release says", se)
        rel["assets"][0]["size"] -= 1
        rel["assets"][0]["digest"] = "sha256:" + "0" * 64
        rc, _, se = self.run_tool("--tag", "repeater-v1.18.0", "--env", "RAK_4631_repeater", "--board", "b",
                                  "--out", str(self.dir / "o"))
        self.assertEqual(rc, 1)
        self.assertIn("doesn't match the release's", se)
        self.assertFalse((self.dir / "o").exists())

    def test_token_goes_to_the_api_only(self):
        gh = mf.GitHub(self.gh.base, token="secret-token")
        rel = gh.release_by_tag(REPO, "repeater-v1.18.0")
        gh.download(mf.pick_asset(rel, "RAK_4631_repeater"), self.dir / "a.zip")
        auth = {p: h.get("Authorization") for p, h in self.gh.requests}
        self.assertEqual(auth[f"/repos/{REPO}/releases/tags/repeater-v1.18.0"], "Bearer secret-token")
        downloads = [a for p, a in auth.items() if p.startswith(("/download/", "/storage/"))]
        self.assertEqual(len(downloads), 2)
        self.assertEqual(downloads, [None, None])

    def test_rate_limit_message(self):
        self.gh.rate_limited = True
        rc, _, se = self.run_tool("--latest", "repeater", "--list")
        self.assertEqual(rc, 1)
        self.assertIn("rate limit", se)
        self.assertIn("GITHUB_TOKEN", se)

    def test_asset_names(self):
        rel = {"assets": [{"name": n, "size": 1, "browser_download_url": "u"} for n in (
            "RAK_4631_repeater-v1.18.0-81dbae5.zip", "ikoka_stick_nrf_22dbm_repeater-v1.18.0-81dbae5.zip",
            "RAK_4631_repeater-v1.18.0-81dbae5.uf2", "Heltec_v3_repeater-v1.18.0-81dbae5-merged.bin",
            "notes.zip")]}
        a = mf.dfu_assets(rel)
        self.assertEqual(sorted(a), ["RAK_4631_repeater", "ikoka_stick_nrf_22dbm_repeater"])
        self.assertEqual(a["RAK_4631_repeater"].firmware_version, "v1.18.0-81dbae5")

    def test_requires_env_board_out(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            mf.main(["--tag", "repeater-v1.18.0", "--env", "x", "--out", "o"])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            mf.main(["--env", "x", "--board", "b", "--out", "o"])


if __name__ == "__main__":
    unittest.main()
