#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for web/staging.js, the installer page's staging-image builder, run in Node.

The page builds the staging partition image in the browser (SPEC.md §8), so it
must produce exactly the bytes dfu32_stage_image.py produces. These tests build
the same packages with both and compare. They need Node 20 or later on PATH and
are skipped without it.

Run: python3 -m unittest discover -s tools -p 'test_*.py'
"""
from __future__ import annotations

import hashlib
import io
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dfu32_stage_image as si  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
STAGING_JS = (REPO / "web" / "staging.js").as_uri()
NODE = shutil.which("node")

# Calls one exported function per request and prints the results as JSON.
# Byte arrays travel as hex both ways.
RUNNER = """
import * as s from %(module)s;
const fromHex = h => Uint8Array.from(h.match(/../g) || [], b => parseInt(b, 16));
const reqs = JSON.parse(await new Promise(r => { let d = ''; process.stdin.on('data', c => d += c); process.stdin.on('end', () => r(d)); }));
const out = [];
for (const q of reqs) {
  try {
    let v;
    if (q.op === 'image') {
      const pkg = await s.loadPackage(fromHex(q.zip));
      const pkt = s.validate(pkg.bin, pkg.dat, q.max ?? s.MAX_APP_SIZE);
      v = { image: s.hex(await s.buildImage(pkg.bin, pkg.dat, q.opts)), deviceType: pkt.deviceType, sdReq: pkt.sdReq };
    } else if (q.op === 'header') {
      const bin = fromHex(q.bin), dat = fromHex(q.dat);
      v = s.hex(await s.buildHeader({ bin, dat, crc16: s.parseDat(dat).crc16, ...q.opts }));
    } else if (q.op === 'md5') v = s.md5Hex(fromHex(q.data));
    else if (q.op === 'crc32') v = s.crc32(fromHex(q.data));
    else if (q.op === 'crc16') v = s.crc16(fromHex(q.data));
    else if (q.op === 'staging') v = s.findStaging(fromHex(q.data));
    else if (q.op === 'parts') v = s.parsePartitionTable(fromHex(q.data));
    else if (q.op === 'guess') v = { board: s.guessBoard(q.name), version: s.guessVersion(q.name) };
    out.push({ ok: v });
  } catch (e) {
    out.push({ err: e.message, stage: e instanceof s.StageError });
  }
}
process.stdout.write(JSON.stringify(out));
"""


def run_js(requests: list[dict]) -> list[dict]:
    script = RUNNER % {"module": json.dumps(STAGING_JS)}
    r = subprocess.run([NODE, "--input-type=module", "-e", script], input=json.dumps(requests),
                       capture_output=True, text=True, timeout=60)
    if r.returncode:
        raise AssertionError(f"node failed:\n{r.stderr}")
    return json.loads(r.stdout)


def one(req: dict) -> dict:
    return run_js([req])[0]


def make_bin(size: int = 4096) -> bytes:
    return bytes((i * 7 + 3) & 0xFF for i in range(size))


def make_dat(bin_data: bytes, device_type: int = 0x0052, sd_req: tuple[int, ...] = (0x00B6,)) -> bytes:
    return struct.pack("<HHIH", device_type, 0xFFFF, 0xFFFFFFFF, len(sd_req)) + \
        struct.pack(f"<{len(sd_req)}H", *sd_req) + struct.pack("<H", si.crc16_ccitt(bin_data))


def make_zip(files: dict[str, bytes], method: int = zipfile.ZIP_DEFLATED) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", method) as z:
        for name, data in files.items():
            z.writestr(name, data)
    return buf.getvalue()


def nrfutil_zip(bin_data: bytes, dat: bytes, method: int = zipfile.ZIP_DEFLATED) -> bytes:
    manifest = {"manifest": {"application": {"bin_file": "app.bin", "dat_file": "app.dat",
                                             "init_packet_data": {"device_type": 82}},
                             "dfu_version": 0.5}}
    return make_zip({"manifest.json": json.dumps(manifest).encode(), "app.bin": bin_data, "app.dat": dat}, method)


def partition_table(entries: list[tuple[str, int, int, int, int]]) -> bytes:
    """ESP-IDF binary partition table: 32-byte entries, the MD5 entry, then 0xFF."""
    raw = b"".join(struct.pack("<HBBII16sI", 0x50AA, t, st, off, size, label.encode(), 0)
                   for label, t, st, off, size in entries)
    raw += b"\xEB\xEB" + b"\xFF" * 14 + hashlib.md5(raw).digest()
    return raw + b"\xFF" * (0xC00 - len(raw))


@unittest.skipUnless(NODE, "node is not installed")
class MatchesPython(unittest.TestCase):
    def test_image_matches_stage_image_tool(self):
        reqs, expected = [], []
        for size, method, dt, sd in [(4096, zipfile.ZIP_DEFLATED, 0x0052, (0x00B6,)),
                                     (786432, zipfile.ZIP_DEFLATED, 0x0052, (0x00B6, 0x0101, 0xFFFE)),
                                     (1000, zipfile.ZIP_STORED, 0x0054, (0xFFFE,))]:
            b = make_bin(size)
            d = make_dat(b, dt, sd)
            opts = {"board": "rak4631", "version": "v1.18.0-81dbae5", "stagedAt": 1762650000}
            reqs.append({"op": "image", "zip": nrfutil_zip(b, d, method).hex(), "opts": opts})
            expected.append((si.build_image(b, d, target_id="", board="rak4631", version="v1.18.0-81dbae5",
                                            staged_at=1762650000), dt, list(sd)))
        for got, (image, dt, sd) in zip(run_js(reqs), expected):
            self.assertIn("ok", got, got)
            self.assertEqual(bytes.fromhex(got["ok"]["image"]), image)
            self.assertEqual(got["ok"]["deviceType"], dt)
            self.assertEqual(got["ok"]["sdReq"], sd)

    def test_zip_without_manifest(self):
        b = make_bin(64)
        d = make_dat(b)
        got = one({"op": "image", "zip": make_zip({"x/fw.bin": b, "x/fw.dat": d}).hex(), "opts": {}})
        self.assertEqual(bytes.fromhex(got["ok"]["image"]),
                         si.build_image(b, d, target_id="", board="", version="", staged_at=0))

    def test_header_with_every_field(self):
        b, opts = make_bin(256), {"board": "heltec_t114", "version": "é" * 31, "targetId": "attic",
                                  "manifestUrl": "usb:RAK_4631_repeater-v1.18.0-81dbae5.zip", "stagedAt": 2 ** 40}
        d = make_dat(b)
        got = one({"op": "header", "bin": b.hex(), "dat": d.hex(), "opts": opts})
        want = si.build_header(si.StagingHeader(
            bin_size=len(b), bin_sha256=hashlib.sha256(b).digest(), dat=d, dat_sha256=hashlib.sha256(d).digest(),
            crc16=si.crc16_ccitt(b), target_id="attic", board="heltec_t114", version="é" * 31,
            staged_at=2 ** 40, manifest_url=opts["manifestUrl"]))
        self.assertEqual(bytes.fromhex(got["ok"]), want)

    def test_checksums(self):
        samples = [b"", b"a", b"abc", bytes(range(256)) * 9, b"\xFF" * 55, b"\x00" * 56, b"\x01" * 64]
        reqs = [{"op": op, "data": s.hex()} for s in samples for op in ("md5", "crc32", "crc16")]
        got = iter(run_js(reqs))
        for s in samples:
            self.assertEqual(next(got)["ok"], hashlib.md5(s).hexdigest())
            self.assertEqual(next(got)["ok"], zlib.crc32(s))
            self.assertEqual(next(got)["ok"], si.crc16_ccitt(s))


@unittest.skipUnless(NODE, "node is not installed")
class Rejects(unittest.TestCase):
    def check(self, zip_bytes: bytes, fragment: str, **req):
        got = one({"op": "image", "zip": zip_bytes.hex(), "opts": {}, **req})
        self.assertIn("err", got, got)
        self.assertTrue(got["stage"], got)
        self.assertIn(fragment, got["err"])

    def test_crc_mismatch(self):
        b = make_bin(64)
        self.check(nrfutil_zip(b, make_dat(make_bin(68))), "CRC16 mismatch")

    def test_unknown_device_type(self):
        b = make_bin(64)
        self.check(nrfutil_zip(b, make_dat(b, device_type=0x0041)), "device_type is 0x0041")

    def test_unaligned_bin(self):
        b = make_bin(63)
        self.check(nrfutil_zip(b, make_dat(b)), "not a multiple of 4")

    def test_too_large_for_nrf52(self):
        b = make_bin(64)
        self.check(nrfutil_zip(b, make_dat(b)), "exceeds the nRF52 app limit", max=60)

    def test_softdevice_package(self):
        m = {"manifest": {"softdevice_bootloader": {"bin_file": "sd.bin", "dat_file": "sd.dat"}}}
        self.check(make_zip({"manifest.json": json.dumps(m).encode()}), "has no application")

    def test_not_a_zip(self):
        self.check(b"PK not really" * 10, "not a zip file")

    def test_corrupt_entry(self):
        b = make_bin(4096)
        z = bytearray(nrfutil_zip(b, make_dat(b), zipfile.ZIP_STORED))
        i = z.find(b[:64])
        z[i + 100] ^= 0xFF
        self.check(bytes(z), "CRC mismatch")

    def test_signed_init_packet(self):
        b = make_bin(64)
        self.check(nrfutil_zip(b, make_dat(b) + b"\x00" * 8), "expected 14")


@unittest.skipUnless(NODE, "node is not installed")
class PartitionTable(unittest.TestCase):
    def test_finds_staging_in_both_layouts(self):
        for csv, offset, size in [("4mb.csv", 0x300000, 0xD0000), ("8mb.csv", 0x630000, 0x100000)]:
            rows = []
            for line in (REPO / "partitions" / csv).read_text().splitlines():
                if not line.strip() or line.startswith("#"):
                    continue
                name, t, st, off, sz = [c.strip() for c in line.split(",")[:5]]
                types = {"app": 0, "data": 1}
                subtypes = {"nvs": 2, "ota": 0, "phy": 1, "coredump": 3, "ota_0": 0x10, "ota_1": 0x11}
                rows.append((name, types[t], subtypes.get(st) if st in subtypes else int(st, 0), int(off, 0), int(sz, 0)))
            got = one({"op": "staging", "data": partition_table(rows).hex()})["ok"]
            self.assertEqual(got, {"type": 1, "subtype": 0x40, "offset": offset, "size": size, "label": "staging"})

    def test_no_staging_partition(self):
        table = partition_table([("nvs", 1, 2, 0x9000, 0x6000), ("factory", 0, 0, 0x10000, 0x100000)])
        self.assertIsNone(one({"op": "staging", "data": table.hex()})["ok"])
        self.assertEqual(len(one({"op": "parts", "data": table.hex()})["ok"]), 2)

    def test_label_must_have_staging_subtype(self):
        table = partition_table([("staging", 1, 0x81, 0x300000, 0xD0000)])
        self.assertIsNone(one({"op": "staging", "data": table.hex()})["ok"])

    def test_erased_flash(self):
        self.assertEqual(one({"op": "parts", "data": ("ff" * 0xC00)})["ok"], [])


@unittest.skipUnless(NODE, "node is not installed")
class Guesses(unittest.TestCase):
    def test_meshcore_names(self):
        cases = {
            "RAK_4631_repeater-v1.18.0-81dbae5.zip": ("rak4631", "v1.18.0-81dbae5"),
            "Heltec_t114_repeater-v1.18.0-81dbae5.zip": ("heltec_t114", "v1.18.0-81dbae5"),
            "Xiao_nrf52_room_server-v1.9.1-abcdef0.zip": ("xiao_nrf52", "v1.9.1-abcdef0"),
            "C:\\Downloads\\LilyGo_T-Echo_companion_radio_ble-v1.18.0-81dbae5.zip": ("lilygo_t_echo", "v1.18.0-81dbae5"),
            "firmware.zip": ("", "firmware"),
            "my build.zip": ("", "my build"),
        }
        got = run_js([{"op": "guess", "name": n} for n in cases])
        for (name, (board, version)), g in zip(cases.items(), got):
            self.assertEqual((g["ok"]["board"], g["ok"]["version"]), (board, version), name)

    def test_version_clipped_to_63_bytes(self):
        self.assertEqual(one({"op": "guess", "name": "x" * 100 + ".zip"})["ok"]["version"], "x" * 63)


if __name__ == "__main__":
    unittest.main()
