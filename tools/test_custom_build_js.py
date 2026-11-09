#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for web/custom-build.js, the installer page's "Custom build" loader, run in Node.

The page installs a build .zip (a CI run's dfu32-<board> artifact) by listing
its parts the way dfu32_web_manifest.py lists a published build's, so these
tests zip a fake build directory, load it with both and compare. They need
Node 20 or later on PATH and are skipped without it.

Run: python3 -m unittest discover -s tools -p 'test_*.py'
"""
from __future__ import annotations

import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_partitions as cp  # noqa: E402
import dfu32_web_manifest as wm  # noqa: E402
from test_dfu32_web_manifest import flasher_args  # noqa: E402
from test_dfu32_web_versions import CHIP_ESP32, CHIP_S3, TABLE_4MB, TABLE_8MB, app_image, merged_image  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
MODULE = (REPO / "web" / "custom-build.js").as_uri()
NODE = shutil.which("node")

RUNNER = """
import * as c from %(module)s;
const fromHex = h => Uint8Array.from(h.match(/../g) || [], b => parseInt(b, 16));
const reqs = JSON.parse(await new Promise(r => { let d = ''; process.stdin.on('data', x => d += x); process.stdin.on('end', () => r(d)); }));
const out = [];
for (const q of reqs) {
  try {
    let v;
    if (q.op === 'load') {
      const b = await c.loadBuild(fromHex(q.zip), q.name || '');
      v = { board: b.board, chipFamily: b.chipFamily, version: b.version, debug: b.debug,
            parts: b.parts.map(p => ({ name: p.name, file: p.file, offset: p.offset, data: Buffer.from(p.data).toString('hex') })),
            manifest: c.buildManifest(b, p => p.file) };
    } else if (q.op === 'chip') v = c.chipFamily(q.chip);
    else if (q.op === 'diff') v = c.layoutDiff(fromHex(q.a), fromHex(q.b));
    else if (q.op === 'sdkconfig') v = c.parseSdkconfig(q.text);
    out.push({ ok: v });
  } catch (e) {
    out.push({ err: e.message });
  }
}
process.stdout.write(JSON.stringify(out));
"""


def run_js(requests: list[dict]) -> list[dict]:
    script = RUNNER % {"module": json.dumps(MODULE)}
    r = subprocess.run([NODE, "--input-type=module", "-e", script], input=json.dumps(requests),
                       capture_output=True, text=True, timeout=60)
    if r.returncode:
        raise AssertionError(f"node failed:\n{r.stderr}")
    return json.loads(r.stdout)


def one(req: dict) -> dict:
    return run_js([req])[0]


class BuildDir:
    """A fake IDF build directory with real-looking images, and the artifact zip CI would upload."""

    def __init__(self, board="feather_esp32s3", chip="esp32s3", version="v0.5.0-2-gabc1234", debug=False,
                 tag_board=None, project="dfu32"):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name) / "build" / board
        (self.dir / "bootloader").mkdir(parents=True)
        (self.dir / "partition_table").mkdir()
        cid, table = (CHIP_ESP32, TABLE_8MB) if chip == "esp32" else (CHIP_S3, TABLE_4MB)
        app = app_image(cid, version.split("-")[0], tag_board or board, project)
        _, parts = merged_image(cid, table, app=app)
        self.files = {}
        for (_, data), rel in zip(parts, ("bootloader/bootloader.bin", "partition_table/partition-table.bin",
                                          "ota_data_initial.bin", "dfu32.bin")):
            self.files[rel] = data
        otadata = next(e for e in table if e[0] == "otadata")[3]
        ota0 = next(e for e in table if e[0] == "ota_0")[3]
        self.files["flasher_args.json"] = json.dumps(
            flasher_args(chip, "0x1000" if chip == "esp32" else "0x0", hex(ota0), hex(otadata))).encode()
        self.files["project_description.json"] = json.dumps({"project_version": version}).encode()
        self.files["sdkconfig"] = (f'CONFIG_IDF_TARGET="{chip}"\nCONFIG_DFU32_BOARD_NAME="{board}"\n' +
                                   ("CONFIG_DFU32_DEBUG_ENDPOINTS=y\n" if debug else "")).encode()
        for rel, data in self.files.items():
            (self.dir / rel).write_bytes(data)
        self.board = board

    def zip(self, prefix=None, drop=(), extra=None) -> bytes:
        """As upload-artifact zips it: paths under build/<board>/, plus the lock files at the root."""
        prefix = f"build/{self.board}/" if prefix is None else prefix
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
            for rel, data in self.files.items():
                if rel not in drop:
                    z.writestr(prefix + rel, data)
            z.writestr(prefix + f"dfu32-{self.board}-merged.bin", b"\xFF" * 4096)
            z.writestr("dependencies.lock.esp32s3", "dependencies: {}\n")
            for name, data in (extra or {}).items():
                z.writestr(name, data)
        return buf.getvalue()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.tmp.cleanup()


@unittest.skipUnless(NODE, "node is not installed")
class MatchesPython(unittest.TestCase):
    def test_same_parts_as_the_manifest_tool(self):
        for board, chip in (("feather_esp32s3", "esp32s3"), ("feather_esp32_v2", "esp32")):
            with self.subTest(board=board), BuildDir(board, chip) as b:
                got = one({"op": "load", "zip": b.zip().hex()})["ok"]
                site = Path(b.tmp.name) / "site"
                py = json.loads(wm.build_manifest(b.dir, board, site).read_text())
                self.assertEqual(got["board"], board)
                self.assertEqual(got["version"], py["version"])
                self.assertEqual(got["manifest"]["builds"][0]["chipFamily"], py["builds"][0]["chipFamily"])
                self.assertEqual([(p["file"], p["offset"]) for p in got["parts"]],
                                 [(Path(p["path"]).name, p["offset"]) for p in py["builds"][0]["parts"]])
                for p, pyp in zip(got["parts"], py["builds"][0]["parts"]):
                    self.assertEqual(bytes.fromhex(p["data"]), (site / pyp["path"]).read_bytes())
                self.assertEqual({k: v for k, v in got["manifest"].items() if k != "builds"},
                                 {k: v for k, v in py.items() if k != "builds"})

    def test_chip_family_matches(self):
        chips = ["esp32", "esp32s3", "esp32c3", "ESP32S2", "esp8266"]
        for chip, got in zip(chips, run_js([{"op": "chip", "chip": c} for c in chips])):
            self.assertEqual(got["ok"], wm.chip_family(chip))
        self.assertIn("unknown chip", one({"op": "chip", "chip": "rp2040"})["err"])

    def test_sdkconfig_matches(self):
        text = 'CONFIG_A="x y"\n# CONFIG_B is not set\nCONFIG_C=y\nCONFIG_D=0x10\nnot a line\n'
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "sdkconfig").write_text(text)
            self.assertEqual(one({"op": "sdkconfig", "text": text})["ok"], wm.read_sdkconfig(Path(d) / "sdkconfig"))

    def test_lists_match(self):
        js = (REPO / "web" / "custom-build.js").read_text()

        def const(name):
            return re.findall(r"'([^']+)'", re.search(rf"export const {name} = \[(.*?)\];", js).group(1))
        self.assertEqual(const("KEPT"), list(cp.KEPT))
        self.assertEqual(const("PARTS"), list(wm.PARTS))


@unittest.skipUnless(NODE, "node is not installed")
class Loads(unittest.TestCase):
    def test_build_directory_at_the_root(self):
        with BuildDir() as b:
            self.assertEqual(one({"op": "load", "zip": b.zip(prefix="").hex()})["ok"]["board"], "feather_esp32s3")

    def test_macos_resource_forks_ignored(self):
        with BuildDir() as b:
            z = b.zip(extra={"__MACOSX/build/feather_esp32s3/._flasher_args.json": b"\0\5\x16\7"})
            self.assertIn("ok", one({"op": "load", "zip": z.hex()}))

    def test_debug_build_flagged(self):
        with BuildDir(debug=True) as b:
            self.assertTrue(one({"op": "load", "zip": b.zip().hex()})["ok"]["debug"])

    def test_version_falls_back_to_the_app(self):
        with BuildDir(version="v0.6.0") as b:
            got = one({"op": "load", "zip": b.zip(drop=("project_description.json",)).hex()})
            self.assertEqual(got["ok"]["version"], "v0.6.0")

    def test_layout_diff(self):
        from test_dfu32_web_versions import partition_table
        moved = [e if e[0] != "runlog" else ("runlog", 1, 0x41, 0x12000, 0xD000) for e in TABLE_4MB]
        a, b = partition_table(TABLE_4MB).hex(), partition_table(moved).hex()
        self.assertEqual(run_js([{"op": "diff", "a": a, "b": a}, {"op": "diff", "a": a, "b": b},
                                 {"op": "diff", "a": a, "b": partition_table(TABLE_8MB).hex()}]),
                         [{"ok": []}, {"ok": ["runlog"]}, {"ok": ["nvs", "runlog", "staging"]}])


@unittest.skipUnless(NODE, "node is not installed")
class Rejects(unittest.TestCase):
    def err(self, zip_bytes: bytes) -> str:
        got = one({"op": "load", "zip": zip_bytes.hex()})
        self.assertIn("err", got, got)
        return got["err"]

    def test_debug_artifact_has_only_the_app(self):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("dfu32-feather_esp32s3-debug.bin", b"\xE9" * 100)
        self.assertIn("dfu32-<board>-debug", self.err(buf.getvalue()))

    def test_two_builds(self):
        with BuildDir() as b:
            extra = {"other/flasher_args.json": b.files["flasher_args.json"]}
            self.assertIn("more than one build", self.err(b.zip(extra=extra)))

    def test_missing_part(self):
        with BuildDir() as b:
            self.assertIn("ota_data_initial.bin", self.err(b.zip(drop=("ota_data_initial.bin",))))

    def test_no_sdkconfig(self):
        with BuildDir() as b:
            self.assertIn("sdkconfig", self.err(b.zip(drop=("sdkconfig",))))

    def test_not_a_dfu32_build(self):
        with BuildDir() as b:
            b.files["sdkconfig"] = b'CONFIG_IDF_TARGET="esp32s3"\n'
            self.assertIn("isn't a DFU32 build", self.err(b.zip()))
        with BuildDir(project="blink") as b:
            self.assertIn("'blink', not DFU32", self.err(b.zip()))

    def test_app_tagged_for_another_board(self):
        with BuildDir(tag_board="esp32s3_n16r8") as b:
            self.assertIn("tagged for esp32s3_n16r8", self.err(b.zip()))

    def test_bad_offsets(self):
        for offset in (None, "", " ", "0x", "abc", -1, 1.5, True):
            with self.subTest(offset=offset), BuildDir() as b:
                args = json.loads(b.files["flasher_args.json"])
                args["otadata"]["offset"] = offset
                b.files["flasher_args.json"] = json.dumps(args).encode()
                self.assertIn("gives 'otadata' the offset", self.err(b.zip()))

    def test_numeric_offsets_accepted(self):
        with BuildDir() as b:
            args = json.loads(b.files["flasher_args.json"])
            args["otadata"]["offset"] = 0xF000
            b.files["flasher_args.json"] = json.dumps(args).encode()
            got = one({"op": "load", "zip": b.zip().hex()})["ok"]
            self.assertEqual(got["parts"][2]["offset"], 0xF000)

    def test_path_outside_the_build_directory(self):
        with BuildDir() as b:
            args = json.loads(b.files["flasher_args.json"])
            args["app"]["file"] = "../../dfu32.bin"
            b.files["flasher_args.json"] = json.dumps(args).encode()
            self.assertIn("outside the build directory", self.err(b.zip(extra={"dfu32.bin": b.files["dfu32.bin"]})))

    def test_dot_dot_inside_the_build_directory(self):
        with BuildDir() as b:
            args = json.loads(b.files["flasher_args.json"])
            args["app"]["file"] = "bootloader/../dfu32.bin"
            b.files["flasher_args.json"] = json.dumps(args).encode()
            self.assertIn("ok", one({"op": "load", "zip": b.zip().hex()}))

    def test_overlapping_parts(self):
        with BuildDir() as b:
            args = json.loads(b.files["flasher_args.json"])
            args["otadata"]["offset"] = "0x8800"  # inside the partition table's 0xC00 bytes
            b.files["flasher_args.json"] = json.dumps(args).encode()
            self.assertIn("overlap", self.err(b.zip()))

    def test_app_without_a_descriptor(self):
        from test_dfu32_web_versions import esp_image
        with BuildDir() as b:
            b.files["dfu32.bin"] = esp_image(CHIP_S3, [b"\0" * 400])
            self.assertIn("esp_app_desc_t", self.err(b.zip()))

    def test_not_a_zip(self):
        self.assertIn("not a zip", self.err(b"\xE9" * 1000))


class Page(unittest.TestCase):
    HTML = (REPO / "web" / "index.html").read_text()

    def test_loaded_by_page(self):
        self.assertIn("import('./custom-build.js')", self.HTML)
        self.assertIn("from './staging.js'", (REPO / "web" / "custom-build.js").read_text())
        self.assertNotRegex((REPO / "web" / "custom-build.js").read_text(), r"https?://")


if __name__ == "__main__":
    unittest.main()
