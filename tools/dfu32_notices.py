#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write THIRD_PARTY_NOTICES: the third-party software in DFU32's firmware and
installer page, with versions and licenses (MILESTONES M5).

Sources:
  - dependencies.lock.<target>: the registry components and the ESP-IDF version
    each firmware build uses;
  - the vendored improv-wifi/sdk-cpp (components/improv/third_party/sdk-cpp);
  - ESP Web Tools, at the version tools/fetch_esp_web_tools.py pins;
  - tools/notices/: ESP-IDF's own copyright list for the locked IDF release
    (docs/en/COPYRIGHT.rst, which names the code ESP-IDF bundles) and the
    license texts that need their copyright lines kept (cJSON).

A component missing from COMPONENTS below, or an ESP-IDF release without its
copyright list in tools/notices/, is an error: a new dependency can't ship
without its license being looked at.

The output is committed at the repository root; CI runs --check so it can't go
stale, and the pages and release jobs ship it.

Examples:
  dfu32_notices.py                    # rewrite THIRD_PARTY_NOTICES
  dfu32_notices.py --check            # CI: fail if it is out of date
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NOTICES_DIR = Path(__file__).resolve().parent / "notices"
IMPROV_SDK = Path("components/improv/third_party/sdk-cpp")
OUTPUT = "THIRD_PARTY_NOTICES"


@dataclass(frozen=True)
class Component:
    license: str
    source: str
    used_for: str
    extra_text: str | None = None  # file in tools/notices/ reproduced in full


# Every registry component in a lock file. Licenses checked against each
# project's LICENSE file and source headers.
COMPONENTS: dict[str, Component] = {
    "espressif/button": Component(
        "Apache-2.0", "https://github.com/espressif/esp-iot-solution/tree/master/components/button",
        "the button (press length: Improv, unclaim, factory reset)"),
    "espressif/cjson": Component(
        "Apache-2.0 (component); MIT (cJSON itself, see below)",
        "https://github.com/espressif/idf-extra-components/tree/master/cjson",
        "JSON parsing (manifests, the HTTP API)", "cJSON-LICENSE"),
    "espressif/cmake_utilities": Component(
        "Apache-2.0", "https://github.com/espressif/esp-iot-solution/tree/master/tools/cmake_utilities",
        "build scripts for button and led_indicator (none of it is in the firmware)"),
    "espressif/led_indicator": Component(
        "Apache-2.0", "https://github.com/espressif/esp-iot-solution/tree/master/components/led/led_indicator",
        "LED blink patterns"),
    "espressif/led_strip": Component(
        "Apache-2.0", "https://github.com/espressif/idf-extra-components/tree/master/led_strip",
        "the NeoPixel"),
    "espressif/mdns": Component(
        "Apache-2.0", "https://github.com/espressif/esp-protocols/tree/master/components/mdns",
        "mDNS (dfu32-xxxx.local)"),
    "h2zero/esp-nimble-cpp": Component(
        "Apache-2.0", "https://github.com/h2zero/esp-nimble-cpp",
        "BLE: the DFU client, scanning and Improv BLE"),
}


class NoticeError(Exception):
    pass


@dataclass
class Lock:
    target: str
    idf: str
    components: dict[str, str]  # name -> version (idf excluded)


def _value(line: str) -> str:
    return line.split(":", 1)[1].strip().strip("'\"")


def parse_lock(text: str, name: str = "lock") -> Lock:
    """Reads the parts of an IDF component-manager lock file needed here: the
    target, and each dependency's own version (indented under its name, not the
    versions inside its nested `dependencies:` list)."""
    target = ""
    versions: dict[str, str | None] = {}
    in_deps = False
    current: str | None = None
    for raw in text.splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        line = raw.strip()
        if indent == 0:
            in_deps = line == "dependencies:"
            current = None
            if line.startswith("target:"):
                target = _value(line)
            continue
        if not in_deps:
            continue
        if indent == 2 and line.endswith(":"):
            current = line[:-1].strip("'\"")
            versions[current] = None
        elif indent == 4 and current is not None and line.startswith("version:"):
            versions[current] = _value(line)
    missing = [n for n, v in versions.items() if not v]
    if missing:
        raise NoticeError(f"{name}: no version for {', '.join(missing)}")
    if not target:
        raise NoticeError(f"{name}: no target")
    if "idf" not in versions:
        raise NoticeError(f"{name}: no idf entry")
    idf = versions.pop("idf")
    assert idf is not None
    return Lock(target, idf, {n: v for n, v in versions.items() if v is not None})


def find_locks(root: Path) -> list[Path]:
    locks = sorted(root.glob("dependencies.lock.*"))
    if not locks:
        raise NoticeError(f"no dependencies.lock.* in {root}")
    return locks


def _versions(per_target: dict[str, str]) -> str:
    """'4.2.3', or '4.2.3 (esp32), 4.2.4 (esp32s3)' when the targets differ."""
    distinct = sorted(set(per_target.values()))
    if len(distinct) == 1:
        return distinct[0]
    return ", ".join(f"{v} ({t})" for t, v in sorted(per_target.items()))


def improv_sdk_version(root: Path) -> str:
    readme = (root / IMPROV_SDK / "README.md").read_text(encoding="utf-8")
    m = re.search(r"Version:\s*([0-9][0-9.]*)", readme)
    if not m:
        raise NoticeError(f"{IMPROV_SDK}/README.md: no 'Version:' line")
    return m.group(1).rstrip(".")


def esp_web_tools_version() -> str:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    try:
        import fetch_esp_web_tools
    finally:
        sys.path.pop(0)
    return fetch_esp_web_tools.VERSION


def idf_copyright(idf_version: str, notices_dir: Path = NOTICES_DIR) -> tuple[str, str]:
    """ESP-IDF's copyright list for the locked release (by major.minor)."""
    m = re.match(r"^(\d+)\.(\d+)", idf_version)
    if not m:
        raise NoticeError(f"can't read ESP-IDF version {idf_version!r}")
    name = f"esp-idf-{m.group(1)}.{m.group(2)}-COPYRIGHT.rst"
    path = notices_dir / name
    if not path.is_file():
        raise NoticeError(
            f"ESP-IDF {idf_version} is locked but tools/notices/{name} is missing: copy docs/en/COPYRIGHT.rst "
            f"from that ESP-IDF release there")
    return name, path.read_text(encoding="utf-8")


def heading(title: str, char: str = "-") -> str:
    return f"{title}\n{char * len(title)}\n"


def render(root: Path = ROOT, locks: list[Path] | None = None, notices_dir: Path = NOTICES_DIR,
           ewt_version: str | None = None) -> str:
    lock_paths = locks if locks is not None else find_locks(root)
    parsed = [parse_lock(p.read_text(encoding="utf-8"), p.name) for p in lock_paths]

    idf_versions = {lk.target: lk.idf for lk in parsed}
    if len(set(idf_versions.values())) != 1:
        raise NoticeError(f"the lock files name different ESP-IDF versions: {idf_versions}")
    idf = parsed[0].idf

    per_component: dict[str, dict[str, str]] = {}
    for lk in parsed:
        for name, ver in lk.components.items():
            per_component.setdefault(name, {})[lk.target] = ver
    unknown = sorted(n for n in per_component if n not in COMPONENTS)
    if unknown:
        raise NoticeError(
            f"no license entry for {', '.join(unknown)}: add it to COMPONENTS in tools/dfu32_notices.py")

    copyright_name, copyright_text = idf_copyright(idf, notices_dir)
    apache = (root / IMPROV_SDK / "LICENSE").read_text(encoding="utf-8")
    ewt = ewt_version or esp_web_tools_version()

    out: list[str] = []
    out.append(heading("DFU32 third-party notices", "="))
    out.append(
        "DFU32 itself is MIT-licensed (see LICENSE). Its firmware and its browser\n"
        "installer page include the third-party software listed here.\n\n"
        "Generated by tools/dfu32_notices.py from the component lock files\n("
        + ", ".join(p.name for p in lock_paths)
        + "); do not edit by hand.\n")

    out.append(heading("Firmware"))
    out.append(
        f"ESP-IDF {idf}\n"
        "  License:  Apache-2.0, plus the bundled third-party code listed under\n"
        "            \"ESP-IDF copyrights\" below\n"
        "  Source:   https://github.com/espressif/esp-idf\n"
        "  Used for: the framework (FreeRTOS, WiFi, lwIP, NimBLE, Mbed TLS, the console)\n")
    for name in sorted(per_component):
        c = COMPONENTS[name]
        out.append(
            f"{name} {_versions(per_component[name])}\n"
            f"  License:  {c.license}\n"
            f"  Source:   {c.source}\n"
            f"  Used for: {c.used_for}\n")
    out.append(
        f"improv-wifi/sdk-cpp {improv_sdk_version(root)} (vendored in {IMPROV_SDK.as_posix()})\n"
        "  License:  Apache-2.0\n"
        "  Source:   https://github.com/improv-wifi/sdk-cpp\n"
        "  Used for: Improv Wi-Fi packets (Serial and BLE)\n")

    out.append(heading("Installer page"))
    out.append(
        f"ESP Web Tools {ewt} (served from vendor/esp-web-tools-{ewt}/, with its LICENSE)\n"
        "  License:  Apache-2.0 (the libraries bundled into its build keep their own\n"
        "            licenses; see its repository)\n"
        "  Source:   https://github.com/esphome/esp-web-tools\n"
        "  Used for: flashing over Web Serial and Improv Serial in the browser\n")

    out.append(heading("ESP-IDF copyrights"))
    out.append(f"ESP-IDF's own list (tools/notices/{copyright_name}, from its docs/en/COPYRIGHT.rst):\n")
    out.append(copyright_text.rstrip("\n") + "\n")

    extras = sorted({t for n in per_component if (t := COMPONENTS[n].extra_text)})
    for extra in extras:
        out.append(heading(f"{extra.replace('-LICENSE', '')} license"))
        out.append((notices_dir / extra).read_text(encoding="utf-8").rstrip("\n") + "\n")

    out.append(heading("Apache License 2.0"))
    out.append(apache.rstrip("\n") + "\n")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", type=Path, default=ROOT, help="repository root (default: this checkout)")
    ap.add_argument("--out", type=Path, help=f"output file (default: ROOT/{OUTPUT})")
    ap.add_argument("--check", action="store_true", help="fail if the output file is not up to date")
    args = ap.parse_args(argv)
    out = args.out or args.root / OUTPUT
    try:
        text = render(args.root)
    except (NoticeError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if args.check:
        current = out.read_text(encoding="utf-8") if out.is_file() else None
        if current != text:
            print(f"error: {out} is out of date: run tools/dfu32_notices.py", file=sys.stderr)
            return 1
        print(f"{out} is up to date")
        return 0
    out.write_text(text, encoding="utf-8")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
