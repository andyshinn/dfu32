#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a DFU32 staging-partition image from an nRF52 legacy DFU package.

The output is the raw content of the `staging` partition (SPEC.md §8): a 4 KiB
header sector followed by the application .bin. Flash it to the ESP32 with
parttool.py or esptool (the command is printed at the end).

Inputs are either an adafruit-nrfutil DFU .zip (the MeshCore release format)
or a separate firmware.bin + firmware.dat. The image is validated the same way
DFU32 validates it before touching a target: .dat layout, device type 0x0052,
word alignment, size limit and CRC16 of the .bin against the .dat.

Examples:
  dfu32_stage_image.py RAK_4631_repeater-v1.18.0-81dbae5.zip --board rak4631 -o staging.img
  dfu32_stage_image.py --bin firmware.bin --dat firmware.dat --board xiao -o staging.img
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import time
import zipfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = b"DF32STG1"
HEADER_VERSION = 1
HEADER_SIZE = 0x240
DATA_OFFSET = 0x1000
MAX_DAT = 64
MAX_APP_SIZE = 802816  # nRF52840 + S140 6.1.1; S140 7.3.0 is 798720
ADAFRUIT_DEVICE_TYPE = 0x0052
STAGING_SIZES = {"4mb": 0xD0000, "8mb": 0x100000}
STAGING_OFFSETS = {"4mb": 0x300000, "8mb": 0x630000}


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE, same as Nordic crc16_compute()."""
    for b in data:
        crc = ((crc >> 8) | (crc << 8)) & 0xFFFF
        crc ^= b
        crc ^= (crc & 0xFF) >> 4
        crc ^= (crc << 12) & 0xFFFF
        crc ^= ((crc & 0xFF) << 5) & 0xFFFF
    return crc


@dataclass
class InitPacket:
    device_type: int
    device_rev: int
    app_version: int
    sd_req: list[int]
    crc16: int


def parse_dat(dat: bytes) -> InitPacket:
    if len(dat) < 14:
        raise ValueError(f".dat too short ({len(dat)} bytes)")
    device_type, device_rev, app_version, n = struct.unpack_from("<HHIH", dat, 0)
    if n == 0:
        raise ValueError(".dat has an empty sd_req list (bootloader rejects it)")
    expected = 12 + 2 * n
    if len(dat) != expected:
        raise ValueError(f".dat is {len(dat)} bytes, expected {expected} for dfu-version 0.5 "
                         "(extended/signed init packets are not supported)")
    sd_req = list(struct.unpack_from(f"<{n}H", dat, 10))
    (crc,) = struct.unpack_from("<H", dat, 10 + 2 * n)
    return InitPacket(device_type, device_rev, app_version, sd_req, crc)


@dataclass
class StagingHeader:
    bin_size: int
    bin_sha256: bytes
    dat: bytes
    dat_sha256: bytes
    crc16: int
    target_id: str = ""
    board: str = ""
    version: str = ""
    manifest_sha256: bytes = field(default=b"\x00" * 32)
    staged_at: int = 0
    manifest_url: str = ""


def _str_field(s: str, size: int, name: str) -> bytes:
    raw = s.encode("utf-8")
    if len(raw) >= size:
        raise ValueError(f"{name} too long ({len(raw)} bytes, max {size - 1})")
    return raw.ljust(size, b"\x00")


def build_header(h: StagingHeader) -> bytes:
    if not 0 < len(h.dat) <= MAX_DAT:
        raise ValueError("dat size out of range")
    buf = bytearray(HEADER_SIZE)
    buf[0x000:0x008] = MAGIC
    struct.pack_into("<I", buf, 0x008, HEADER_VERSION)
    struct.pack_into("<I", buf, 0x00C, h.bin_size)
    buf[0x010:0x030] = h.bin_sha256
    struct.pack_into("<H", buf, 0x030, len(h.dat))
    buf[0x032:0x032 + len(h.dat)] = h.dat
    buf[0x072:0x092] = h.dat_sha256
    struct.pack_into("<H", buf, 0x092, h.crc16)
    buf[0x094:0x0B4] = _str_field(h.target_id, 32, "target_id")
    buf[0x0B4:0x0D4] = _str_field(h.board, 32, "board")
    buf[0x0D4:0x114] = _str_field(h.version, 64, "version")
    buf[0x114:0x134] = h.manifest_sha256
    struct.pack_into("<Q", buf, 0x134, h.staged_at)
    buf[0x13C:0x23C] = _str_field(h.manifest_url, 256, "manifest_url")
    struct.pack_into("<I", buf, 0x23C, zlib.crc32(bytes(buf[:0x23C])) & 0xFFFFFFFF)
    return bytes(buf)


def load_package(zip_path: Path | None, bin_path: Path | None, dat_path: Path | None) -> tuple[bytes, bytes]:
    if zip_path:
        with zipfile.ZipFile(zip_path) as z:
            names = z.namelist()
            bin_name = dat_name = None
            if "manifest.json" in names:
                app = json.loads(z.read("manifest.json")).get("manifest", {}).get("application")
                if not app:
                    raise ValueError("zip manifest has no 'application' entry (SD/BL packages are not supported)")
                bin_name, dat_name = app["bin_file"], app["dat_file"]
            else:
                bins = [n for n in names if n.endswith(".bin")]
                dats = [n for n in names if n.endswith(".dat")]
                if len(bins) != 1 or len(dats) != 1:
                    raise ValueError("zip has no manifest.json and not exactly one .bin and .dat")
                bin_name, dat_name = bins[0], dats[0]
            return z.read(bin_name), z.read(dat_name)
    if not (bin_path and dat_path):
        raise ValueError("give a DFU .zip, or both --bin and --dat")
    return bin_path.read_bytes(), dat_path.read_bytes()


def validate(bin_data: bytes, dat: bytes, max_app_size: int) -> InitPacket:
    pkt = parse_dat(dat)
    if pkt.device_type != ADAFRUIT_DEVICE_TYPE:
        raise ValueError(f".dat device_type is 0x{pkt.device_type:04X}; the Adafruit bootloader requires 0x0052")
    if not bin_data:
        raise ValueError("empty .bin")
    if len(bin_data) % 4:
        raise ValueError(f".bin size {len(bin_data)} is not a multiple of 4")
    if len(bin_data) > max_app_size:
        raise ValueError(f".bin size {len(bin_data)} exceeds max app size {max_app_size}")
    crc = crc16_ccitt(bin_data)
    if crc != pkt.crc16:
        raise ValueError(f"CRC16 mismatch: .bin is 0x{crc:04X}, .dat says 0x{pkt.crc16:04X}")
    return pkt


def build_image(bin_data: bytes, dat: bytes, *, target_id: str, board: str, version: str,
                staged_at: int) -> bytes:
    pkt = parse_dat(dat)
    header = build_header(StagingHeader(
        bin_size=len(bin_data),
        bin_sha256=hashlib.sha256(bin_data).digest(),
        dat=dat,
        dat_sha256=hashlib.sha256(dat).digest(),
        crc16=pkt.crc16,
        target_id=target_id,
        board=board,
        version=version,
        staged_at=staged_at,
    ))
    return header + b"\xFF" * (DATA_OFFSET - HEADER_SIZE) + bin_data


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("zip", nargs="?", type=Path, help="adafruit-nrfutil DFU .zip")
    ap.add_argument("--bin", type=Path, help="application .bin (instead of a zip)")
    ap.add_argument("--dat", type=Path, help="init packet .dat (instead of a zip)")
    ap.add_argument("-o", "--output", type=Path, required=True, help="staging image to write")
    ap.add_argument("--board", default="", help="board id recorded in the header (e.g. rak4631)")
    ap.add_argument("--version", default="", help="version string recorded in the header")
    ap.add_argument("--target-id", default="", help="DFU32 target id recorded in the header")
    ap.add_argument("--layout", choices=sorted(STAGING_SIZES), default="4mb",
                    help="DFU32 partition layout, for size check and printed offsets (default 4mb)")
    ap.add_argument("--max-app-size", type=lambda s: int(s, 0), default=MAX_APP_SIZE)
    args = ap.parse_args(argv)

    try:
        bin_data, dat = load_package(args.zip, args.bin, args.dat)
        pkt = validate(bin_data, dat, args.max_app_size)
        version = args.version or (args.zip.stem if args.zip else "")
        image = build_image(bin_data, dat, target_id=args.target_id, board=args.board,
                            version=version[:63], staged_at=int(time.time()))
    except (ValueError, KeyError, OSError, zipfile.BadZipFile) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if len(image) > STAGING_SIZES[args.layout]:
        print(f"error: image ({len(image)} B) exceeds the {args.layout} staging partition", file=sys.stderr)
        return 1
    args.output.write_bytes(image)

    sd = ", ".join(f"0x{s:04X}" for s in pkt.sd_req)
    print(f"bin      {len(bin_data)} B  sha256 {hashlib.sha256(bin_data).hexdigest()}")
    print(f"dat      {len(dat)} B  device_type 0x{pkt.device_type:04X}  sd_req [{sd}]  crc16 0x{pkt.crc16:04X} (matches)")
    print(f"wrote    {args.output} ({len(image)} B)")
    print()
    print("Flash it to the DFU32 staging partition (ESP-IDF environment active, ESP32 on PORT):")
    print(f"  python -m esptool --port PORT write-flash 0x{STAGING_OFFSETS[args.layout]:X} {args.output}")
    print("or by partition name:")
    print(f"  python $IDF_PATH/components/partition_table/parttool.py --port PORT "
          f"write_partition --partition-name staging --input {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
