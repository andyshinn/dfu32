#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for dfu32_stage_image.py. Run: python3 -m unittest discover -s tools -p 'test_*.py'

The golden header (test/host/fixtures/staging_header_golden.bin) is shared with
the C++ host tests, so the Python writer and C++ reader agree on the layout.
Regenerate it with: python3 tools/test_stage_image.py --regen-golden
"""
from __future__ import annotations

import hashlib
import io
import os
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dfu32_stage_image as si  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "test" / "host" / "fixtures"
GOLDEN = FIXTURES / "staging_header_golden.bin"


def golden_header() -> si.StagingHeader:
    dat = (FIXTURES / "meshcore-v1.18.0-rak_4631.dat").read_bytes()
    return si.StagingHeader(
        bin_size=440924,
        bin_sha256=hashlib.sha256(b"dfu32-golden-bin").digest(),
        dat=dat,
        dat_sha256=hashlib.sha256(dat).digest(),
        crc16=18911,
        target_id="attic-repeater",
        board="rak4631",
        version="v1.18.0-81dbae5",
        manifest_sha256=hashlib.sha256(b"dfu32-golden-manifest").digest(),
        staged_at=1793750400,
        manifest_url="http://fw.lan/meshcore/rak4631-repeater/manifest.json",
    )


def synthetic_package(size: int = 1024, device_type: int = 0x0052) -> tuple[bytes, bytes]:
    bin_data = bytes((i * 7 + 3) & 0xFF for i in range(size))
    crc = si.crc16_ccitt(bin_data)
    dat = bytes([device_type & 0xFF, device_type >> 8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00,
                 0xB6, 0x00, crc & 0xFF, crc >> 8])
    return bin_data, dat


class Crc16(unittest.TestCase):
    def test_check_value(self):
        self.assertEqual(si.crc16_ccitt(b"123456789"), 0x29B1)


class Dat(unittest.TestCase):
    def test_fixture(self):
        pkt = si.parse_dat((FIXTURES / "meshcore-v1.18.0-xiao_nrf52.dat").read_bytes())
        self.assertEqual(pkt.device_type, 0x0052)
        self.assertEqual(pkt.sd_req, [0x0123])
        self.assertEqual(pkt.crc16, 57502)

    def test_extended_rejected(self):
        with self.assertRaises(ValueError):
            si.parse_dat((FIXTURES / "meshcore-v1.18.0-rak_4631.dat").read_bytes() + b"\x00\x00")


class Header(unittest.TestCase):
    def test_matches_golden(self):
        self.assertEqual(si.build_header(golden_header()), GOLDEN.read_bytes())

    def test_layout(self):
        h = si.build_header(golden_header())
        self.assertEqual(len(h), si.HEADER_SIZE)
        self.assertEqual(h[:8], b"DF32STG1")

    def test_field_too_long(self):
        hdr = golden_header()
        hdr.board = "x" * 32
        with self.assertRaises(ValueError):
            si.build_header(hdr)


class Image(unittest.TestCase):
    def test_build_and_validate(self):
        bin_data, dat = synthetic_package()
        si.validate(bin_data, dat, si.MAX_APP_SIZE)
        img = si.build_image(bin_data, dat, target_id="t", board="b", version="v", staged_at=0)
        self.assertEqual(len(img), si.DATA_OFFSET + len(bin_data))
        self.assertEqual(img[si.DATA_OFFSET:], bin_data)
        self.assertTrue(all(b == 0xFF for b in img[si.HEADER_SIZE:si.DATA_OFFSET]))

    def test_validation_errors(self):
        bin_data, dat = synthetic_package()
        with self.assertRaisesRegex(ValueError, "CRC16"):
            si.validate(bin_data[:-4] + b"\x00\x00\x00\x00", dat, si.MAX_APP_SIZE)
        with self.assertRaisesRegex(ValueError, "multiple of 4"):
            si.validate(bin_data + b"\x00", dat, si.MAX_APP_SIZE)
        with self.assertRaisesRegex(ValueError, "exceeds"):
            si.validate(bin_data, dat, 512)
        with self.assertRaisesRegex(ValueError, "device_type"):
            si.validate(bin_data, b"\xFF\xFF" + dat[2:], si.MAX_APP_SIZE)

    def test_nrf54_device_type(self):
        bin_data, dat = synthetic_package(2048, device_type=0x0054)
        si.validate(bin_data, dat, 512)  # nRF52 ceiling does not apply to nRF54 images

    def test_cli_with_zip(self):
        bin_data, dat = synthetic_package(2048)
        with tempfile.TemporaryDirectory() as tmp:
            zpath = Path(tmp) / "pkg.zip"
            with zipfile.ZipFile(zpath, "w") as z:
                z.writestr("manifest.json", '{"manifest":{"application":{"bin_file":"a.bin","dat_file":"a.dat"}}}')
                z.writestr("a.bin", bin_data)
                z.writestr("a.dat", dat)
            out = Path(tmp) / "staging.img"
            with io.StringIO() as buf:
                old, sys.stdout = sys.stdout, buf
                try:
                    rc = si.main([str(zpath), "-o", str(out), "--board", "rak4631"])
                finally:
                    sys.stdout = old
            self.assertEqual(rc, 0)
            self.assertEqual(out.read_bytes()[si.DATA_OFFSET:], bin_data)

    @unittest.skipUnless(os.environ.get("DFU32_TEST_ZIP"), "DFU32_TEST_ZIP not set")
    def test_real_zip(self):
        bin_data, dat = si.load_package(Path(os.environ["DFU32_TEST_ZIP"]), None, None)
        si.validate(bin_data, dat, si.MAX_APP_SIZE)


class FakeDfu32:
    """A DFU32 that checks POST /api/v1/staging the way the firmware does (SPEC.md §8, §10.1)."""

    def __init__(self, token: str | None = "tok"):  # None: token auth off, as DFU32 ships
        import http.server
        import json
        import threading
        import urllib.parse
        fake = self
        self.requests: list[dict] = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def reply(self, code: int, obj: dict):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                u = urllib.parse.urlparse(self.path)
                q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                fake.requests.append({"path": u.path, "query": q, "body": body,
                                      "auth": self.headers.get("Authorization")})
                if token is not None and self.headers.get("Authorization") != "Bearer " + token:
                    return self.reply(401, {"error": "UNAUTHORIZED"})
                pkt = si.parse_dat(bytes.fromhex(q["dat"]))
                if si.crc16_ccitt(body) != pkt.crc16:
                    return self.reply(422, {"error": "DAT_CRC_MISMATCH", "detail": "CRC16 of the .bin does not match the .dat"})
                if hashlib.sha256(body).hexdigest() != q.get("sha256", hashlib.sha256(body).hexdigest()):
                    return self.reply(422, {"error": "HASH_MISMATCH"})
                self.reply(200, {"valid": True, "version": q["version"], "board": q.get("board", ""),
                                 "bin_size": len(body), "manifest_url": "upload:" + q.get("name", "")})

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Upload(unittest.TestCase):
    def setUp(self):
        self.dfu32 = FakeDfu32()
        self.addCleanup(self.dfu32.close)
        # A proxy configured in the environment must not intercept the local fake.
        env = {k: v for k, v in os.environ.items() if k.lower() not in ("http_proxy", "https_proxy", "all_proxy")}
        env["no_proxy"] = env["NO_PROXY"] = "127.0.0.1,localhost"
        from unittest import mock
        patcher = mock.patch.dict(os.environ, env, clear=True)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def zip_of(self, bin_data: bytes, dat: bytes, name: str) -> Path:
        zpath = Path(self.tmp.name) / name
        with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("manifest.json", '{"manifest":{"application":{"bin_file":"a.bin","dat_file":"a.dat"}}}')
            z.writestr("a.bin", bin_data)
            z.writestr("a.dat", dat)
        return zpath

    def run_cli(self, *args: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        old = sys.stdout, sys.stderr
        sys.stdout, sys.stderr = out, err
        try:
            rc = si.main(list(args))
        finally:
            sys.stdout, sys.stderr = old
        return rc, out.getvalue(), err.getvalue()

    def test_uploads_bin_with_dat_in_the_query(self):
        bin_data, dat = synthetic_package(4096)
        z = self.zip_of(bin_data, dat, "RAK_4631_repeater-v1.18.0-81dbae5.zip")
        rc, out, err = self.run_cli(str(z), "--upload", self.dfu32.url, "--token", "tok", "--board", "rak4631")
        self.assertEqual(rc, 0, err)
        r = self.dfu32.requests[-1]
        self.assertEqual(r["path"], "/api/v1/staging")
        self.assertEqual(r["body"], bin_data)
        self.assertEqual(r["query"]["dat"], dat.hex())
        self.assertEqual(r["query"]["version"], "v1.18.0-81dbae5")  # MeshCore name: <version>-<commit>
        self.assertEqual(r["query"]["board"], "rak4631")
        self.assertEqual(r["query"]["sha256"], hashlib.sha256(bin_data).hexdigest())
        self.assertEqual(r["query"]["name"], "RAK_4631_repeater-v1.18.0-81dbae5.zip")
        self.assertIn("staged   v1.18.0-81dbae5", out)

    def test_token_from_the_environment_and_host_without_scheme(self):
        bin_data, dat = synthetic_package(1024)
        os.environ["DFU32_TOKEN"] = "tok"
        host = self.dfu32.url.removeprefix("http://") + "/"
        rc, _, err = self.run_cli("--bin", str(self.zip_of(bin_data, dat, "x.zip")), "--upload", host)
        self.assertEqual(rc, 1)  # --bin without --dat
        (Path(self.tmp.name) / "fw.bin").write_bytes(bin_data)
        (Path(self.tmp.name) / "fw.dat").write_bytes(dat)
        rc, _, err = self.run_cli("--bin", str(Path(self.tmp.name) / "fw.bin"), "--dat", str(Path(self.tmp.name) / "fw.dat"),
                                  "--upload", host, "--version", "my build")
        self.assertEqual(rc, 0, err)
        r = self.dfu32.requests[-1]
        self.assertEqual(r["query"]["version"], "my build")
        self.assertNotIn("board", r["query"])
        self.assertEqual(r["query"]["name"], "fw.bin")

    def test_refusals_are_reported(self):
        bin_data, dat = synthetic_package(1024)
        z = self.zip_of(bin_data, dat, "pkg.zip")
        rc, _, err = self.run_cli(str(z), "--upload", self.dfu32.url, "--token", "wrong")
        self.assertEqual(rc, 1)
        self.assertIn("HTTP 401", err)
        rc, _, err = self.run_cli(str(z), "--upload", self.dfu32.url)
        self.assertEqual(rc, 1)
        self.assertIn("requires an API token", err)
        self.assertIsNone(self.dfu32.requests[-1]["auth"])  # no header at all without a token

    def test_device_without_a_token(self):
        open_dfu32 = FakeDfu32(token=None)
        self.addCleanup(open_dfu32.close)
        bin_data, dat = synthetic_package(1024)
        rc, _, err = self.run_cli(str(self.zip_of(bin_data, dat, "pkg.zip")), "--upload", open_dfu32.url)
        self.assertEqual(rc, 0, err)
        self.assertIsNone(open_dfu32.requests[-1]["auth"])

    def test_checked_before_uploading(self):
        bin_data, dat = synthetic_package(1024)
        _, other = synthetic_package(512)
        rc, _, err = self.run_cli(str(self.zip_of(bin_data, other, "bad.zip")), "--upload", self.dfu32.url, "--token", "tok")
        self.assertEqual(rc, 1)
        self.assertIn("CRC16 mismatch", err)
        self.assertEqual(self.dfu32.requests, [])

    def test_unreachable(self):
        bin_data, dat = synthetic_package(1024)
        self.dfu32.close()
        rc, _, err = self.run_cli(str(self.zip_of(bin_data, dat, "pkg.zip")), "--upload", self.dfu32.url, "--token", "tok")
        self.assertEqual(rc, 1)
        self.assertIn("cannot reach", err)

    def test_output_or_upload_is_required(self):
        with self.assertRaises(SystemExit), contextlib_redirect():
            si.main(["pkg.zip"])

    def test_query_stays_inside_the_uri_limit(self):
        # The worst case: the longest .dat, a 63-byte version and a board, a long non-ASCII name.
        url = si.upload_url("dfu32-ab12.local", dat=b"\xff" * 64, version="é" * 31, board="b" * 31,
                            sha256=b"\0" * 32, name="ü" * 200 + ".zip")
        self.assertLess(len(url.split("dfu32-ab12.local", 1)[1]), 1024 - len("POST  HTTP/1.1"))

    def test_guess_version(self):
        self.assertEqual(si.guess_version("RAK_4631_repeater-v1.18.0-81dbae5.zip"), "v1.18.0-81dbae5")
        self.assertEqual(si.guess_version("/tmp/firmware.zip"), "firmware")
        self.assertEqual(len(si.guess_version("x" * 100 + ".zip").encode()), 63)


def contextlib_redirect():
    import contextlib
    return contextlib.redirect_stderr(io.StringIO())


if __name__ == "__main__":
    if "--regen-golden" in sys.argv:
        GOLDEN.write_bytes(si.build_header(golden_header()))
        print(f"wrote {GOLDEN}")
    else:
        unittest.main()
