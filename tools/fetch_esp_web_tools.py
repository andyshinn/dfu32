#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Copy ESP Web Tools into the installer site, from a hash-pinned npm tarball.

The installer page loads ESP Web Tools from its own origin rather than a CDN
(SPEC.md §9.4). The bundle drives flashing over Web Serial and loads its
dialog and flasher stubs with dynamic import(), which an `integrity` attribute
on the entry script would not cover; pinning the tarball's SHA-256 here covers
every file. The tarball's LICENSE (Apache-2.0) is copied alongside.

Writes SITE/vendor/esp-web-tools-<VERSION>/ (dist/web/*.js and LICENSE). The
page's <script> path names the same version; test_dfu32_web_manifest.py checks
that they agree.

Examples:
  fetch_esp_web_tools.py --site _site       # CI pages job
  fetch_esp_web_tools.py --site web         # local preview (web/vendor/ is git-ignored)
  fetch_esp_web_tools.py --site web --tarball esp-web-tools-10.4.0.tgz

To upgrade: change VERSION and SHA256 (sha256sum of the `npm pack` tarball)
together, and update the <script> path in web/index.html.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path

VERSION = "10.4.0"
SHA256 = "f18da75335d2f0dca044c4bb052848c0696e7b03cc23d61d8530dd6eba0a9008"
URL = f"https://registry.npmjs.org/esp-web-tools/-/esp-web-tools-{VERSION}.tgz"
WEB_DIR = "package/dist/web/"


class FetchError(Exception):
    pass


def vendor_dir(site: Path) -> Path:
    return site / "vendor" / f"esp-web-tools-{VERSION}"


def install(site: Path, data: bytes, sha256: str = SHA256) -> Path:
    """Verify the tarball bytes and unpack the browser bundle into SITE. Returns the vendor directory."""
    got = hashlib.sha256(data).hexdigest()
    if got != sha256:
        raise FetchError(f"tarball SHA-256 {got} does not match the pinned {sha256}")
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
    out = vendor_dir(site)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    for name, content in files.items():
        (out / name).write_bytes(content)
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--site", required=True, type=Path, help="site directory (the one holding index.html)")
    ap.add_argument("--tarball", type=Path, help=f"use this file instead of downloading {URL}")
    a = ap.parse_args(argv)
    try:
        if a.tarball:
            data = a.tarball.read_bytes()
        else:
            with urllib.request.urlopen(URL, timeout=60) as r:
                data = r.read()
        out = install(a.site, data)
    except (FetchError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"ESP Web Tools {VERSION} -> {out} ({len(list(out.iterdir()))} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
