# improv-wifi/sdk-cpp (vendored)

- Upstream: https://github.com/improv-wifi/sdk-cpp
- Version: 1.2.7 (`library.json`), copied unmodified from `main` on 2026-11-08: `src/improv.h`, `src/improv.cpp`, `LICENSE`.
- License: Apache-2.0 (see `LICENSE`). DFU32 itself is MIT; SPEC.md §16.

It isn't in the ESP Component Registry, so it is vendored rather than listed in an
`idf_component.yml`. DFU32 uses only the packet parser and the RPC response builder;
the transports are DFU32's own (`components/improv`).

`parse_improv_data()` indexes one byte past the end when the SSID length points at
the last byte, and its checksum loop uses a `uint8_t` index. `dfu32::improv::Core`
therefore caps packets at 200 bytes and passes a copy with one spare zero byte at
the end, so the upstream file stays unmodified.
