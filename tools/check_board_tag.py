#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks that a built dfu32.bin carries the board tag the firmware upload expects.

    check_board_tag.py build/feather_esp32s3/dfu32.bin feather_esp32s3

POST /api/v1/system/ota (SPEC.md §10.1) reads DFU32's board tag at a fixed
offset: right after esp_app_desc_t, where ESP-IDF's linker script places
.rodata_custom_desc. If a toolchain or IDF change ever moves it, every
legitimate upload would be refused as "untagged". Running this on the real
image in CI turns that into a build failure instead. The offsets mirror
components/dfu32_core/include/dfu32_core/ota_image.hpp.
"""
import struct
import sys
from pathlib import Path

IMAGE_MAGIC = 0xE9
APP_DESC_OFFSET = 24 + 8          # image header + first segment header
APP_DESC_MAGIC = 0xABCD5432
BOARD_TAG_OFFSET = APP_DESC_OFFSET + 256
BOARD_TAG_MAGIC = b"DF32BRD1"


def cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def read_tag(image: bytes) -> dict:
    """Returns the fields the device checks, or raises ValueError."""
    if len(image) < BOARD_TAG_OFFSET + 40:
        raise ValueError("image too short")
    if image[0] != IMAGE_MAGIC:
        raise ValueError("not an ESP-IDF app image (first byte 0x%02x)" % image[0])
    (magic,) = struct.unpack_from("<I", image, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        raise ValueError("esp_app_desc_t not at offset %d" % APP_DESC_OFFSET)
    tag = image[BOARD_TAG_OFFSET:BOARD_TAG_OFFSET + 40]
    if tag[:8] != BOARD_TAG_MAGIC:
        raise ValueError("no DFU32 board tag at offset %d (found %r)" % (BOARD_TAG_OFFSET, tag[:8]))
    return {
        "chip_id": struct.unpack_from("<H", image, 12)[0],
        "version": cstr(image[APP_DESC_OFFSET + 16:APP_DESC_OFFSET + 48]),
        "project": cstr(image[APP_DESC_OFFSET + 48:APP_DESC_OFFSET + 80]),
        "board": cstr(tag[8:40]),
    }


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    image, board = Path(argv[0]).read_bytes(), argv[1]
    try:
        info = read_tag(image)
    except ValueError as e:
        print("error: %s: %s" % (argv[0], e), file=sys.stderr)
        return 1
    if info["project"] != "dfu32" or info["board"] != board:
        print("error: %s: project %r, board %r; expected 'dfu32', %r" % (argv[0], info["project"], info["board"], board),
              file=sys.stderr)
        return 1
    print("%s: dfu32 %s for %s (chip id %d)" % (argv[0], info["version"], info["board"], info["chip_id"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
