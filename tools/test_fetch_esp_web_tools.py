#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for fetch_esp_web_tools.py. Run: python3 -m unittest discover -s tools -p 'test_*.py'"""
from __future__ import annotations

import contextlib
import hashlib
import io
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_esp_web_tools as fewt  # noqa: E402


def tarball(files: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return buf.getvalue()


# What web/stage-dialog.js relies on, as the minified bundle spells it.
HOOKS = ("".join(f'customElements.define("{e}",X{i});' for i, e in enumerate(fewt.HOOK_ELEMENTS))
         + 'html`${na}\n            <div slot="headline">Logs & Console</div>\n          </ew-list-item>`').encode()

GOOD = {
    "package/dist/web/install-button.js": b"import('./chunk.js')",
    "package/dist/web/chunk.js": HOOKS,
    "package/dist/install-button.js": b"not the browser bundle",
    "package/dist/web/sub/nested.js": b"skipped",
    "package/LICENSE": b"Apache License",
    "package/README.md": b"readme",
}


class Install(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.site = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_copies_bundle_and_license(self):
        data = tarball(GOOD)
        out = fewt.install(self.site, data, hashlib.sha256(data).hexdigest())
        self.assertEqual(out, self.site / "vendor" / f"esp-web-tools-{fewt.VERSION}")
        self.assertEqual(sorted(p.name for p in out.iterdir()), ["LICENSE", "chunk.js", "install-button.js"])
        self.assertEqual((out / "install-button.js").read_bytes(), b"import('./chunk.js')")

    def test_missing_dialog_element(self):
        files = dict(GOOD, **{"package/dist/web/chunk.js": HOOKS.replace(b'"ew-filled-text-field"', b'"ew-other"')})
        data = tarball(files)
        with self.assertRaisesRegex(fewt.FetchError, "no longer defines ew-filled-text-field"):
            fewt.install(self.site, data, hashlib.sha256(data).hexdigest())
        self.assertFalse((self.site / "vendor").exists())

    def test_missing_logs_item(self):
        files = dict(GOOD, **{"package/dist/web/chunk.js": HOOKS.replace(b"Logs & Console", b"Console")})
        data = tarball(files)
        with self.assertRaisesRegex(fewt.FetchError, "Logs & Console"):
            fewt.install(self.site, data, hashlib.sha256(data).hexdigest())

    def test_logs_label_no_longer_alone_in_its_item(self):
        # The page matches the item's whole text, so an icon or extra text in it would hide the feature.
        for changed in (HOOKS.replace(b"Logs & Console</div>", b"Logs & Console</div><span>beta</span>"),
                        HOOKS.replace(b"Logs & Console", b"Logs &amp; Console"),
                        b'customElements.define("x",Y);"Logs & Console"' + HOOKS.split(b"html`")[0]):
            data = tarball(dict(GOOD, **{"package/dist/web/chunk.js": changed}))
            with self.assertRaisesRegex(fewt.FetchError, "list item"):
                fewt.install(self.site, data, hashlib.sha256(data).hexdigest())

    def test_failed_write_keeps_old_files(self):
        data = tarball(GOOD)
        out = fewt.install(self.site, data, hashlib.sha256(data).hexdigest())
        with mock.patch.object(Path, "write_bytes", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                fewt.install(self.site, data, hashlib.sha256(data).hexdigest())
        self.assertEqual((out / "install-button.js").read_bytes(), b"import('./chunk.js')")

    def test_replaces_stale_files(self):
        data = tarball(GOOD)
        out = fewt.vendor_dir(self.site)
        out.mkdir(parents=True)
        (out / "stale.js").write_text("old")
        fewt.install(self.site, data, hashlib.sha256(data).hexdigest())
        self.assertFalse((out / "stale.js").exists())

    def test_hash_mismatch(self):
        with self.assertRaisesRegex(fewt.FetchError, "does not match"):
            fewt.install(self.site, tarball(GOOD), "0" * 64)
        self.assertFalse((self.site / "vendor").exists())

    def test_missing_entry(self):
        data = tarball({"package/LICENSE": b"x"})
        with self.assertRaisesRegex(fewt.FetchError, "install-button.js"):
            fewt.install(self.site, data, hashlib.sha256(data).hexdigest())

    def test_pinned_hash_shape(self):
        self.assertRegex(fewt.SHA256, r"^[0-9a-f]{64}$")
        self.assertIn(fewt.VERSION, fewt.URL)
        self.assertRegex(fewt.ESPTOOL_JS_SHA256, r"^[0-9a-f]{64}$")
        self.assertIn(fewt.ESPTOOL_JS_VERSION, fewt.ESPTOOL_JS_URL)


class InstallEsptoolJs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.site = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_copies_bundle_and_license(self):
        data = tarball({"package/bundle.js": b"export {ESPLoader}", "package/lib/index.js": b"lib",
                        "package/LICENSE": b"Apache License", "package/README.md": b"readme"})
        out = fewt.install_esptool_js(self.site, data, hashlib.sha256(data).hexdigest())
        self.assertEqual(out, self.site / "vendor" / f"esptool-js-{fewt.ESPTOOL_JS_VERSION}")
        self.assertEqual(sorted(p.name for p in out.iterdir()), ["LICENSE", "bundle.js"])

    def test_hash_mismatch(self):
        data = tarball({"package/bundle.js": b"x", "package/LICENSE": b"x"})
        with self.assertRaisesRegex(fewt.FetchError, "does not match"):
            fewt.install_esptool_js(self.site, data, "0" * 64)

    def test_missing_bundle(self):
        data = tarball({"package/lib/index.js": b"x", "package/LICENSE": b"x"})
        with self.assertRaisesRegex(fewt.FetchError, "bundle.js"):
            fewt.install_esptool_js(self.site, data, hashlib.sha256(data).hexdigest())



class Main(unittest.TestCase):
    """main(): nothing is written unless both tarballs check out."""

    def test_bad_esptool_js_leaves_site_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            site, d = Path(tmp) / "site", Path(tmp)
            ewt = d / "ewt.tgz"
            ewt.write_bytes(tarball(GOOD))
            ejs = d / "ejs.tgz"
            ejs.write_bytes(tarball({"package/bundle.js": b"x", "package/LICENSE": b"x"}))
            with mock.patch.object(fewt, "SHA256", hashlib.sha256(ewt.read_bytes()).hexdigest()), \
                    contextlib.redirect_stderr(io.StringIO()) as err:
                rc = fewt.main(["--site", str(site), "--tarball", str(ewt), "--esptool-js-tarball", str(ejs)])
            self.assertEqual(rc, 1)
            self.assertIn("does not match", err.getvalue())
            self.assertFalse((site / "vendor").exists())


if __name__ == "__main__":
    unittest.main()
