# DFU32

DFU32 is ESP32 firmware that flashes nRF52 boards over BLE using Nordic **legacy** DFU (the Adafruit nRF52 bootloader and the OTAFIX fork). It pulls firmware from your own server and replaces the phone app, so a remote repeater can be updated without anyone standing next to it.

DFU32 is firmware-agnostic. The motivating use case is MeshCore LoRa repeaters.

**Status:** specification phase. No implementation yet.

- [SPEC.md](SPEC.md): the full specification
- [docs/MILESTONES.md](docs/MILESTONES.md): phased plan
- [docs/OPEN_QUESTIONS.md](docs/OPEN_QUESTIONS.md): open questions and assumptions, ranked
- [docs/PROTOCOL_NOTES.md](docs/PROTOCOL_NOTES.md): verified protocol facts, with source citations

License: MIT
