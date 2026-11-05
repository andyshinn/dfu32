// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "dfu_proto/bootloader_info.hpp"

using dfu::BootloaderClass;
using dfu::classify_bootloader;

TEST_CASE("OTAFIX on a RAK4631 (operator's repeater)", "[blinfo]") {
    const auto i = classify_bootloader("0.9.2-OTAFIX2.2-BP1.3 s140 6.1.1");
    CHECK(i.cls == BootloaderClass::Otafix);
    CHECK(i.version == "0.9.2-OTAFIX2.2-BP1.3");
    CHECK(i.major == 0);
    CHECK(i.minor == 9);
    CHECK(i.patch == 2);
    CHECK(i.sd_variant == "s140");
    CHECK(i.sd_version == "6.1.1");
    CHECK(i.sd_fwid == 0x00B6);
    CHECK(i.device_type == 0x0052);
    CHECK(dfu::max_packet_for(i.cls) == 244);
}

TEST_CASE("nRF54 port on a XIAO nRF54LM20A (bench probe)", "[blinfo]") {
    const auto i = classify_bootloader("v0.5.0-dev2 s145 10.0.1");
    CHECK(i.cls == BootloaderClass::Nrf54);
    CHECK(i.major == 0);
    CHECK(i.minor == 5);
    CHECK(i.patch == 0);
    CHECK(i.sd_variant == "s145");
    CHECK(i.sd_version == "10.0.1");
    CHECK(i.sd_fwid == 0);  // no FWID table for S145; bootloader checks sd_req itself
    CHECK(i.device_type == 0x0054);
    CHECK(dfu::max_packet_for(i.cls) == 244);
}

TEST_CASE("Leading v on an nRF52 version string", "[blinfo]") {
    const auto i = classify_bootloader("v0.11.0 s140 7.3.0");
    CHECK(i.cls == BootloaderClass::UpstreamGe0_10);
    CHECK(i.device_type == 0x0052);
}

TEST_CASE("Upstream versions", "[blinfo]") {
    CHECK(classify_bootloader("0.11.0 s140 7.3.0").cls == BootloaderClass::UpstreamGe0_10);
    CHECK(classify_bootloader("0.11.0 s140 7.3.0").sd_fwid == 0x0123);
    CHECK(classify_bootloader("0.10.0-3-gabcdef s140 6.1.1").cls == BootloaderClass::UpstreamGe0_10);
    CHECK(classify_bootloader("1.0.0 s140 6.1.1").cls == BootloaderClass::UpstreamGe0_10);
    CHECK(classify_bootloader("0.9.2 s140 6.1.1").cls == BootloaderClass::UpstreamLe0_9);
    CHECK(classify_bootloader("0.6.1 s140 6.1.1").cls == BootloaderClass::UpstreamLe0_9);
    CHECK(dfu::max_packet_for(BootloaderClass::UpstreamLe0_9) == 20);
}

TEST_CASE("Git describe suffixes and dirty trees", "[blinfo]") {
    const auto i = classify_bootloader("0.9.2-OTAFIX2.3-BP1.4-2-g3a75482-dirty s140 6.1.1");
    CHECK(i.cls == BootloaderClass::Otafix);
    CHECK(i.sd_fwid == 0x00B6);
}

TEST_CASE("Unknown and malformed strings", "[blinfo]") {
    CHECK(classify_bootloader("").cls == BootloaderClass::Unknown);
    CHECK(classify_bootloader("").device_type == 0);
    CHECK(classify_bootloader("garbage").cls == BootloaderClass::Unknown);
    CHECK(classify_bootloader("v1").cls == BootloaderClass::Unknown);
    const auto no_sd = classify_bootloader("0.9.2");
    CHECK(no_sd.cls == BootloaderClass::UpstreamLe0_9);
    CHECK(no_sd.sd_fwid == 0);
    CHECK(classify_bootloader("0.9.2 s140 9.9.9").sd_fwid == 0);
    CHECK(dfu::max_packet_for(BootloaderClass::Unknown) == 20);
}
