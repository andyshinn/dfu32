# SPDX-License-Identifier: MIT
"""esptool wrapper and the DFU32 flash layouts (partitions/*.csv)."""
from __future__ import annotations

import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional


@dataclass(frozen=True)
class Layout:
    name: str
    app: int
    staging: int


LAYOUTS = {
    "4mb": Layout("4mb", app=0x20000, staging=0x300000),
    "8mb": Layout("8mb", app=0x30000, staging=0x630000),
}


def layout_for_staging(addr: int) -> Optional[Layout]:
    for lay in LAYOUTS.values():
        if lay.staging == addr:
            return lay
    return None


class FlashError(RuntimeError):
    pass


class Flasher:
    def __init__(self, port: str, *, baud: int = 460800, esptool: str = "", log: Optional[Callable[[str], None]] = None):
        self.port = port
        self.baud = baud
        self.cmd = shlex.split(esptool) if esptool else [sys.executable, "-m", "esptool"]
        self.log = log or print

    def write(self, items: list[tuple[int, Path]], *, erase_all: bool = False) -> None:
        """Writes images at offsets; esptool resets the chip into the app afterwards."""
        for _, p in items:
            if not Path(p).is_file():
                raise FlashError(f"{p}: no such file")
        if erase_all:
            self._run(["erase-flash"], ["erase_flash"])
        args = []
        for off, p in items:
            args += [hex(off), str(p)]
        self._run(["write-flash", *args], ["write_flash", *args])

    def _run(self, new_style: list[str], old_style: list[str]) -> None:
        # esptool v5 spells commands with dashes; v4 with underscores.
        for sub in (new_style, old_style):
            argv = [*self.cmd, "--port", self.port, "--baud", str(self.baud), *sub]
            self.log("$ " + " ".join(shlex.quote(a) for a in argv))
            try:
                p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            except FileNotFoundError as e:
                raise FlashError(f"cannot run esptool ({e}); install it (pip install esptool) or pass --esptool") from e
            for line in p.stdout.splitlines():
                self.log("  " + line)
            if p.returncode == 0:
                return
            if "invalid choice" in p.stdout and sub is new_style:
                continue
            raise FlashError(f"esptool failed (exit {p.returncode}). If it fails right after 'Changing baud rate', "
                             "retry with --flash-baud 115200; if it can't connect, hold BOOT and tap RESET.")
        raise FlashError("esptool rejected both command spellings")
