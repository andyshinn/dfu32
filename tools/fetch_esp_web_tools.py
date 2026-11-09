#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Copy ESP Web Tools and esptool-js into the installer site, from hash-pinned npm tarballs.

The installer page loads both from its own origin rather than a CDN (SPEC.md
§9.4). ESP Web Tools drives installing over Web Serial and loads its dialog
and flasher stubs with dynamic import(), which an `integrity` attribute on the
entry script would not cover; pinning each tarball's SHA-256 here covers every
file. esptool-js (its self-contained ESM bundle) writes an nRF52 image to the
staging partition from the page's "Stage nRF52 image" dialog. Each tarball's
LICENSE (Apache-2.0) is copied alongside.

Writes SITE/vendor/esp-web-tools-<VERSION>/ (dist/web/*.js and LICENSE) and
SITE/vendor/esptool-js-<ESPTOOL_JS_VERSION>/ (bundle.js and LICENSE). The
page's paths name the same versions; test_dfu32_web_manifest.py checks that
they agree.

The staging dialog adds an item to ESP Web Tools' device dialog and reuses its
dialog components (web/stage-dialog.js), which are internals rather than an
API. So install() also checks that the pinned bundle still defines those
elements and still has the "Logs & Console" item the new one goes before: an
upgrade that changes them fails here, in the pages job, not on the live page.

Examples:
  fetch_esp_web_tools.py --site _site       # CI pages job
  fetch_esp_web_tools.py --site web         # local preview (web/vendor/ is git-ignored)
  fetch_esp_web_tools.py --site web --tarball esp-web-tools-10.4.0.tgz --esptool-js-tarball esptool-js-0.7.0.tgz

To upgrade: change VERSION and SHA256 (or ESPTOOL_JS_VERSION and
ESPTOOL_JS_SHA256; sha256sum of the `npm pack` tarball) together, and update
the path in web/index.html (or web/stage-dialog.js).
"""
from __future__ import annotations

import argparse
import hashlib
import io
import re
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path

VERSION = "10.4.0"
SHA256 = "f18da75335d2f0dca044c4bb052848c0696e7b03cc23d61d8530dd6eba0a9008"
URL = f"https://registry.npmjs.org/esp-web-tools/-/esp-web-tools-{VERSION}.tgz"
WEB_DIR = "package/dist/web/"

ESPTOOL_JS_VERSION = "0.7.0"
ESPTOOL_JS_SHA256 = "9c7797cfbcac3a989ab85c52c9daaa65ffc0d538c273a055757b98bb846fdf1f"
ESPTOOL_JS_URL = f"https://registry.npmjs.org/esptool-js/-/esptool-js-{ESPTOOL_JS_VERSION}.tgz"

# ESP Web Tools internals that web/stage-dialog.js relies on: the elements it
# renders its own dialog with, and the device dialog's last fixed menu item,
# which the page finds as an ew-list-item whose whole text is that label.
HOOK_ELEMENTS = ("ew-dialog", "ew-icon-button", "ew-list", "ew-list-item", "ew-text-button",
                 "ew-filled-text-field", "ewt-page-progress", "ewt-page-message", "ewt-install-dialog")
HOOK_LABEL = "Logs & Console"
# The item as the bundle's Lit template spells it: the label alone in the
# headline slot, closing the list item.
HOOK_ITEM = re.compile(r'<div slot="headline">\s*' + re.escape(HOOK_LABEL) + r'\s*</div>\s*</ew-list-item>')


class FetchError(Exception):
    pass


def vendor_dir(site: Path) -> Path:
    return site / "vendor" / f"esp-web-tools-{VERSION}"


def esptool_js_dir(site: Path) -> Path:
    return site / "vendor" / f"esptool-js-{ESPTOOL_JS_VERSION}"


def _verify(data: bytes, sha256: str) -> None:
    got = hashlib.sha256(data).hexdigest()
    if got != sha256:
        raise FetchError(f"tarball SHA-256 {got} does not match the pinned {sha256}")


def _write(out: Path, files: dict[str, bytes]) -> Path:
    """Write FILES as directory OUT, replacing it. They go to a sibling first,
    so a failed write leaves the old directory in place."""
    tmp = out.with_name(out.name + ".partial")
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    for name, content in files.items():
        (tmp / name).write_bytes(content)
    if out.exists():
        shutil.rmtree(out)
    tmp.rename(out)
    return out


def check_hooks(files: dict[str, bytes]) -> None:
    """Fail if the ESP Web Tools bundle lacks what web/stage-dialog.js uses.

    A text heuristic over the minified bundle, not a parse: it looks for the
    literal customElements.define("name", ...) calls and the item's template
    markup. A registration written another way (an aliased define) would fail
    here although the element exists, and a match inside a string or comment
    would pass although it doesn't. Either way the page degrades quietly (no
    menu item, a console warning), so a false alarm here is the safer error.
    """
    js = b"\n".join(v for k, v in files.items() if k.endswith(".js")).decode("utf-8", "replace")
    defined = set(re.findall(r"customElements\.define\(\s*[\"']([a-z0-9-]+)[\"']", js))
    missing = [e for e in HOOK_ELEMENTS if e not in defined]
    if missing:
        raise FetchError(f"ESP Web Tools no longer defines {', '.join(missing)}: update web/stage-dialog.js")
    if not HOOK_ITEM.search(js):
        raise FetchError(f"ESP Web Tools' device dialog has no '{HOOK_LABEL}' list item: update web/stage-dialog.js")


def unpack(data: bytes, sha256: str = SHA256) -> dict[str, bytes]:
    """Verify the ESP Web Tools tarball and return its browser bundle and LICENSE by file name."""
    _verify(data, sha256)
    files = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        for m in tar.getmembers():
            if not m.isfile():
                continue
            if m.name.startswith(WEB_DIR) and "/" not in m.name[len(WEB_DIR):]:
                name = m.name[len(WEB_DIR):]
            elif m.name == "package/LICENSE":
                name = "LICENSE"
            else:
                continue
            files[name] = tar.extractfile(m).read()
    if "install-button.js" not in files or "LICENSE" not in files:
        raise FetchError("tarball has no dist/web/install-button.js or LICENSE")
    check_hooks(files)
    return files


def unpack_esptool_js(data: bytes, sha256: str = ESPTOOL_JS_SHA256) -> dict[str, bytes]:
    """Verify the esptool-js tarball and return its ESM bundle and LICENSE by file name."""
    _verify(data, sha256)
    files = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        for m in tar.getmembers():
            if m.isfile() and m.name in ("package/bundle.js", "package/LICENSE"):
                files[m.name[len("package/"):]] = tar.extractfile(m).read()
    if set(files) != {"bundle.js", "LICENSE"}:
        raise FetchError("esptool-js tarball has no bundle.js or LICENSE")
    return files


def install(site: Path, data: bytes, sha256: str = SHA256) -> Path:
    """Verify the tarball bytes and unpack the browser bundle into SITE. Returns the vendor directory."""
    return _write(vendor_dir(site), unpack(data, sha256))


def install_esptool_js(site: Path, data: bytes, sha256: str = ESPTOOL_JS_SHA256) -> Path:
    """Verify the esptool-js tarball and copy its ESM bundle into SITE. Returns the vendor directory."""
    return _write(esptool_js_dir(site), unpack_esptool_js(data, sha256))


def _read(tarball: Path | None, url: str) -> bytes:
    if tarball:
        return tarball.read_bytes()
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--site", required=True, type=Path, help="site directory (the one holding index.html)")
    ap.add_argument("--tarball", type=Path, help=f"use this file instead of downloading {URL}")
    ap.add_argument("--esptool-js-tarball", type=Path, help=f"use this file instead of downloading {ESPTOOL_JS_URL}")
    a = ap.parse_args(argv)
    try:
        # Both fetched and checked before either is written, so a failure
        # leaves the site as it was.
        ewt_files = unpack(_read(a.tarball, URL), SHA256)
        ejs_files = unpack_esptool_js(_read(a.esptool_js_tarball, ESPTOOL_JS_URL), ESPTOOL_JS_SHA256)
        out = _write(vendor_dir(a.site), ewt_files)
        ejs = _write(esptool_js_dir(a.site), ejs_files)
    except (FetchError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"ESP Web Tools {VERSION} -> {out} ({len(list(out.iterdir()))} files)")
    print(f"esptool-js {ESPTOOL_JS_VERSION} -> {ejs}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
