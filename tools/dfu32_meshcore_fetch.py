#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Download a MeshCore nRF52 release and pack it into a DFU32 artifact directory.

MeshCore publishes every firmware type (companion, repeater, room server) as
its own GitHub release, tagged `<kind>-v<version>`, so `releases/latest` can
point at any of them. Each nRF52 build is a DFU zip named
`<env>-<version>-<commit>.zip` (e.g. RAK_4631_repeater-v1.18.0-81dbae5.zip),
so the name can't be guessed ahead of time. This script finds the asset through
the GitHub API, checks its size and digest, and hands it to dfu32_pack.py,
which writes firmware.bin, firmware.dat and manifest.json (SPEC.md §11) into
--out.

The manifest version defaults to `<version>-<commit>` (e.g. v1.18.0-81dbae5),
which is what the repeater's `ver` command prints once it runs the image.

Examples:
  dfu32_meshcore_fetch.py --tag repeater-v1.18.0 --env RAK_4631_repeater --board rak4631 --out site/rak4631
  dfu32_meshcore_fetch.py --latest repeater --env Xiao_nrf52_repeater --board xiao_nrf52 --out site/xiao
  dfu32_meshcore_fetch.py --latest repeater --list      # the nRF52 envs in that release

Unauthenticated, the GitHub API allows 60 requests an hour; set GITHUB_TOKEN
(or GH_TOKEN, or pass --token) if you hit that.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dfu32_pack  # noqa: E402

DEFAULT_REPO = "meshcore-dev/MeshCore"
DEFAULT_API = "https://api.github.com"
USER_AGENT = "dfu32-meshcore-fetch"
PER_PAGE = 100
MAX_PAGES = 5

# build.sh names nRF52 DFU packages "<pio env>-<version>-<short commit>.zip"; the version is the
# part of the tag after its last '-', so it never contains one itself.
ASSET_RE = re.compile(r"^(?P<env>.+)-(?P<version>[^-]+)-(?P<commit>[0-9a-f]{6,40})\.zip$")
# Release tags: "repeater-v1.18.0", "room-server-v1.11.0", "companion-v1.9.1".
TAG_RE = re.compile(r"^(?P<kind>.+)-(?P<version>v?\d+(?:\.\d+)*)$")


class FetchError(Exception):
    pass


@dataclass
class Asset:
    name: str
    env: str
    version: str
    commit: str
    url: str
    size: int
    digest: str = ""

    @property
    def firmware_version(self) -> str:
        """What the image reports with `ver` (FIRMWARE_VERSION_STRING in MeshCore's build.sh)."""
        return f"{self.version}-{self.commit}"


def version_key(v: str) -> tuple:
    return tuple(int(p) for p in v.lstrip("v").split("."))


def _opener() -> urllib.request.OpenerDirector:
    return urllib.request.build_opener()


class GitHub:
    def __init__(self, api: str = DEFAULT_API, token: str = "", opener: Optional[urllib.request.OpenerDirector] = None):
        self.api = api.rstrip("/")
        self.token = token
        self.opener = opener or _opener()

    def get_json(self, path: str):
        req = urllib.request.Request(self.api + path, headers={
            "Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28", "User-Agent": USER_AGENT})
        if self.token:
            req.add_header("Authorization", f"Bearer {self.token}")
        try:
            with self.opener.open(req, timeout=30) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            if e.code in (403, 429) and (e.headers.get("X-RateLimit-Remaining") == "0" or e.code == 429):
                raise FetchError("GitHub API rate limit reached; set GITHUB_TOKEN (or pass --token) and retry") from e
            raise FetchError(f"GitHub API {path}: HTTP {e.code}") from e
        except (urllib.error.URLError, OSError) as e:
            raise FetchError(f"GitHub API {path}: {getattr(e, 'reason', e)}") from e
        except json.JSONDecodeError as e:
            raise FetchError(f"GitHub API {path}: the reply isn't JSON") from e

    def release_by_tag(self, repo: str, tag: str) -> dict:
        try:
            return self.get_json(f"/repos/{repo}/releases/tags/{urllib.parse.quote(tag, safe='')}")
        except FetchError as e:
            if "HTTP 404" in str(e):
                raise FetchError(f"no published release tagged '{tag}' in {repo} (drafts aren't visible)") from e
            raise

    def latest_release(self, repo: str, kind: str, *, prerelease: bool = False) -> dict:
        """The highest `<kind>-v<version>` release. The list comes newest first, so the first page that
        has any release of this kind has the latest one."""
        for page in range(1, MAX_PAGES + 1):
            rels = self.get_json(f"/repos/{repo}/releases?per_page={PER_PAGE}&page={page}")
            if not isinstance(rels, list):
                raise FetchError("GitHub API: unexpected reply listing releases")
            found = []
            for r in rels:
                m = TAG_RE.match(r.get("tag_name") or "")
                if not m or m["kind"] != kind or r.get("draft"):
                    continue
                if r.get("prerelease") and not prerelease:
                    continue
                found.append((version_key(m["version"]), r))
            if found:
                return max(found, key=lambda x: x[0])[1]
            if len(rels) < PER_PAGE:
                break
        raise FetchError(f"no published '{kind}-v…' release in {repo}"
                         + ("" if prerelease else " (pre-releases skipped; --prerelease includes them)"))

    def download(self, asset: Asset, dest: Path) -> None:
        # No Authorization header: the download redirects to a storage host that must not see the token,
        # and public release assets don't need one.
        req = urllib.request.Request(asset.url, headers={"User-Agent": USER_AGENT, "Accept": "application/octet-stream"})
        h = hashlib.sha256()
        n = 0
        try:
            with self.opener.open(req, timeout=60) as r, open(dest, "wb") as f:
                while chunk := r.read(64 * 1024):
                    f.write(chunk)
                    h.update(chunk)
                    n += len(chunk)
        except urllib.error.HTTPError as e:
            raise FetchError(f"downloading {asset.name}: HTTP {e.code}") from e
        except (urllib.error.URLError, OSError) as e:
            raise FetchError(f"downloading {asset.name}: {getattr(e, 'reason', e)}") from e
        if n != asset.size:
            raise FetchError(f"{asset.name}: downloaded {n} B, the release says {asset.size} B")
        algo, _, want = asset.digest.partition(":")
        if algo == "sha256" and want and h.hexdigest() != want.lower():
            raise FetchError(f"{asset.name}: sha256 {h.hexdigest()} doesn't match the release's {want}")


def dfu_assets(release: dict) -> dict[str, Asset]:
    """nRF52 DFU packages in a release, by PlatformIO env name."""
    out: dict[str, Asset] = {}
    for a in release.get("assets") or []:
        m = ASSET_RE.match(a.get("name") or "")
        if not m:
            continue
        out[m["env"]] = Asset(a["name"], m["env"], m["version"], m["commit"], a["browser_download_url"],
                              int(a.get("size") or 0), a.get("digest") or "")
    return out


def pick_asset(release: dict, env: str) -> Asset:
    assets = dfu_assets(release)
    if env in assets:
        return assets[env]
    folded = {k.lower(): v for k, v in assets.items()}
    if env.lower() in folded:
        return folded[env.lower()]
    tag = release.get("tag_name", "?")
    others = [a.get("name", "") for a in release.get("assets") or [] if (a.get("name") or "").startswith(env + "-")]
    if others:
        raise FetchError(f"'{env}' in {tag} has no DFU .zip ({', '.join(others)}): DFU32 flashes nRF52 boards only")
    close = difflib.get_close_matches(env, list(assets), n=5, cutoff=0.5)
    hint = f"; did you mean {', '.join(close)}?" if close else "; --list shows them"
    raise FetchError(f"no nRF52 DFU package for '{env}' in {tag}{hint}")


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    which = ap.add_mutually_exclusive_group(required=True)
    which.add_argument("--tag", help="release tag, e.g. repeater-v1.18.0")
    which.add_argument("--latest", metavar="KIND", help="newest release of this kind: repeater, room-server, companion")
    ap.add_argument("--prerelease", action="store_true", help="with --latest: consider pre-releases too")
    ap.add_argument("--env", help="PlatformIO env of the board, e.g. RAK_4631_repeater (see --list)")
    ap.add_argument("--list", action="store_true", help="list the release's nRF52 envs and exit")
    ap.add_argument("--board", action="append", default=[],
                    help="DFU32 board id the image is for (repeatable); must match the target's board")
    ap.add_argument("--out", type=Path, help="directory to write (created if missing)")
    ap.add_argument("--version", default="", help="manifest version (default: <version>-<commit>, as `ver` prints it)")
    ap.add_argument("--name", default="", help="display name (default: 'MeshCore <kind> (<env>)')")
    ap.add_argument("--keep-zip", type=Path, metavar="DIR", help="also keep the downloaded .zip in DIR")
    ap.add_argument("--max-app-size", default="", help="passed to dfu32_pack.py")
    ap.add_argument("--repo", default=DEFAULT_REPO, help=f"GitHub repository (default {DEFAULT_REPO})")
    ap.add_argument("--api-url", default=DEFAULT_API, help=argparse.SUPPRESS)
    ap.add_argument("--token", default=os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN") or "",
                    help="GitHub token for a higher API rate limit (default: GITHUB_TOKEN or GH_TOKEN)")
    args = ap.parse_args(argv)
    if not args.list:
        for need in ("env", "out"):
            if not getattr(args, need):
                ap.error(f"--{need} is required (or --list)")
        if not args.board:
            ap.error("--board is required (DFU32 refuses images not listed for its target's board)")

    gh = GitHub(args.api_url, args.token)
    try:
        rel = gh.release_by_tag(args.repo, args.tag) if args.tag else \
            gh.latest_release(args.repo, args.latest, prerelease=args.prerelease)
        tag = rel.get("tag_name", "")
        print(f"release  {tag}  {rel.get('name') or ''}  ({(rel.get('published_at') or '')[:10]})".rstrip())
        if args.list:
            envs = sorted(dfu_assets(rel), key=str.lower)
            print(f"{len(envs)} nRF52 DFU package(s):")
            for e in envs:
                print(f"  {e}")
            return 0
        asset = pick_asset(rel, args.env)
        print(f"asset    {asset.name}  {asset.size} B" + (f"  {asset.digest}" if asset.digest else ""))
        m = TAG_RE.match(tag)
        kind = m["kind"].replace("-", " ") + " " if m else ""
        name = args.name or f"MeshCore {kind}({asset.env})"
        with tempfile.TemporaryDirectory(prefix="dfu32-fetch-") as tmp:
            zip_path = Path(tmp) / asset.name
            gh.download(asset, zip_path)
            if args.keep_zip:
                args.keep_zip.mkdir(parents=True, exist_ok=True)
                shutil.copy2(zip_path, args.keep_zip / asset.name)
            pack_argv = [str(zip_path), "--out", str(args.out), "--version", args.version or asset.firmware_version,
                         "--name", name, "--notes", rel.get("html_url") or tag]
            for b in args.board:
                pack_argv += ["--board", b]
            if args.max_app_size:
                pack_argv += ["--max-app-size", args.max_app_size]
            return dfu32_pack.main(pack_argv)
    except FetchError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
