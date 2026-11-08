# SPDX-License-Identifier: MIT
"""Tests for tools/check_board_tag.py (offsets shared with dfu32_core/ota_image.hpp)."""
import io
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_board_tag as cbt  # noqa: E402


def image(board=b"feather_esp32s3", project=b"dfu32", magic=b"DF32BRD1", first=0xE9, desc_magic=0xABCD5432):
    v = bytearray(400)
    v[0] = first
    struct.pack_into("<H", v, 12, 9)
    struct.pack_into("<I", v, cbt.APP_DESC_OFFSET, desc_magic)
    v[cbt.APP_DESC_OFFSET + 16:cbt.APP_DESC_OFFSET + 16 + 6] = b"v0.4.0"
    v[cbt.APP_DESC_OFFSET + 48:cbt.APP_DESC_OFFSET + 48 + len(project)] = project
    if magic:
        v[cbt.BOARD_TAG_OFFSET:cbt.BOARD_TAG_OFFSET + 8] = magic
        v[cbt.BOARD_TAG_OFFSET + 8:cbt.BOARD_TAG_OFFSET + 8 + len(board)] = board
    return bytes(v)


class TestBoardTag(unittest.TestCase):
    def run_main(self, data, board):
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(data)
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = cbt.main([f.name, board])
        Path(f.name).unlink()
        return rc, out.getvalue() + err.getvalue()

    def test_offsets_match_the_firmware_header(self):
        # Keep in step with ota_image.hpp: kAppDescOffset 32, kBoardTagOffset 288.
        hpp = (Path(__file__).resolve().parent.parent / "components/dfu32_core/include/dfu32_core/ota_image.hpp").read_text()
        self.assertEqual(cbt.APP_DESC_OFFSET, 32)
        self.assertEqual(cbt.BOARD_TAG_OFFSET, 288)
        self.assertIn("// 288", hpp)
        self.assertIn("'D', 'F', '3', '2', 'B', 'R', 'D', '1'", hpp)

    def test_good_image(self):
        info = cbt.read_tag(image())
        self.assertEqual(info, {"chip_id": 9, "version": "v0.4.0", "project": "dfu32", "board": "feather_esp32s3"})
        rc, text = self.run_main(image(), "feather_esp32s3")
        self.assertEqual(rc, 0, text)

    def test_failures(self):
        for data, board, needle in [
            (image(magic=None), "feather_esp32s3", "no DFU32 board tag"),
            (image(), "esp32s3_n16r8", "expected"),
            (image(project=b"blink"), "feather_esp32s3", "expected"),
            (image(first=0xFF), "feather_esp32s3", "not an ESP-IDF app image"),
            (image(desc_magic=0), "feather_esp32s3", "esp_app_desc_t not at offset"),
            (image()[:300], "feather_esp32s3", "too short"),
        ]:
            rc, text = self.run_main(data, board)
            self.assertEqual(rc, 1, needle)
            self.assertIn(needle, text)


if __name__ == "__main__":
    unittest.main()
