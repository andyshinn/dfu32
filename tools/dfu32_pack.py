#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turn an nRF52 legacy DFU package into a DFU32 artifact directory.

Writes firmware.bin, firmware.dat and manifest.json (SPEC.md §11) into --out,
ready to serve over plain HTTP. Point the DFU32 target's manifest URL at
<server>/<dir>/manifest.json. The image is validated exactly like DFU32 does
before touching a target (.dat layout, device type, word alignment, size,
CRC16), so a bad package fails here instead of on the device.

Examples:
  dfu32_pack.py RAK_4631_repeater-v1.18.0-81dbae5.zip --board rak4631 --out site/rak4631
  dfu32_pack.py --bin firmware.bin --dat firmware.dat --board xiao_nrf52 --version v1.18.0 --out site/xiao
  python3 -m http.server -d site 8000     # then on DFU32: target url http://<pc>:8000/rak4631/manifest.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dfu32_stage_image as si  # noqa: E402

SCHEMA = 1
MAX_VERSION = 63
MAX_BOARD = 31


def build_manifest(bin_data: bytes, dat: bytes, pkt: si.InitPacket, *, boards: list[str], version: str,
                   name: str = "", notes: str = "", created: str | None = None,
                   bin_name: str = "firmware.bin", dat_name: str = "firmware.dat") -> dict:
    if not boards:
        raise ValueError("at least one --board is required (DFU32 refuses images not listed for its target)")
    for b in boards:
        if not b or len(b) > MAX_BOARD or any(c.isspace() for c in b):
            raise ValueError(f"board '{b}' must be 1-{MAX_BOARD} characters without spaces")
    if not version or len(version.encode()) > MAX_VERSION:
        raise ValueError(f"version must be 1-{MAX_VERSION} bytes")
    m: dict = {
        "schema": SCHEMA,
        "version": version,
        "boards": boards,
        "created": created or time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "bin": {"url": bin_name, "size": len(bin_data), "sha256": hashlib.sha256(bin_data).hexdigest()},
        "dat": {"url": dat_name, "size": len(dat), "sha256": hashlib.sha256(dat).hexdigest()},
        "init": {"device_type": pkt.device_type, "sd_req": pkt.sd_req, "crc16": pkt.crc16},
    }
    if name:
        m["name"] = name
    if notes:
        m["notes"] = notes
    return m


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("zip", nargs="?", type=Path, help="adafruit-nrfutil DFU .zip")
    ap.add_argument("--bin", type=Path, help="application .bin (instead of a zip)")
    ap.add_argument("--dat", type=Path, help="init packet .dat (instead of a zip)")
    ap.add_argument("--board", action="append", default=[], required=True,
                    help="board id the image is for (repeatable); must match the DFU32 target's board")
    ap.add_argument("--version", default="", help="version string (default: the zip's file name)")
    ap.add_argument("--name", default="", help="optional display name, e.g. 'MeshCore repeater'")
    ap.add_argument("--notes", default="", help="optional free text")
    ap.add_argument("--out", type=Path, required=True, help="directory to write (created if missing)")
    ap.add_argument("--max-app-size", type=lambda s: int(s, 0), default=si.MAX_APP_SIZE,
                    help=f"nRF52 application ceiling (default {si.MAX_APP_SIZE})")
    args = ap.parse_args(argv)

    try:
        bin_data, dat = si.load_package(args.zip, args.bin, args.dat)
        pkt = si.validate(bin_data, dat, args.max_app_size)
        version = args.version or (args.zip.stem if args.zip else "")
        manifest = build_manifest(bin_data, dat, pkt, boards=args.board, version=version,
                                  name=args.name, notes=args.notes)
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "firmware.bin").write_bytes(bin_data)
        (args.out / "firmware.dat").write_bytes(dat)
        (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    except (ValueError, KeyError, OSError, zipfile.BadZipFile) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    sd = ", ".join(f"0x{s:04X}" for s in pkt.sd_req)
    print(f"version  {manifest['version']}   boards {', '.join(manifest['boards'])}")
    print(f"bin      {len(bin_data)} B  sha256 {manifest['bin']['sha256']}")
    print(f"dat      {len(dat)} B  device_type 0x{pkt.device_type:04X}  sd_req [{sd}]  crc16 0x{pkt.crc16:04X} (matches)")
    print(f"wrote    {args.out}/manifest.json, firmware.bin, firmware.dat")
    return 0


if __name__ == "__main__":
    sys.exit(main())
