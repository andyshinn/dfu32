// SPDX-License-Identifier: MIT
#include "dfu_proto/bootloader_info.hpp"

#include <cstdlib>
#include <vector>

#include "dfu_proto/chunk.hpp"

namespace dfu {
namespace {

std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        const size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

// Parses up to three leading dot-separated integers; returns count parsed.
int parse_semver(std::string_view s, int& a, int& b, int& c) {
    int* parts[3] = {&a, &b, &c};
    int n = 0;
    size_t i = 0;
    while (n < 3 && i < s.size() && s[i] >= '0' && s[i] <= '9') {
        int v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (s[i] - '0');
            ++i;
        }
        *parts[n++] = v;
        if (i < s.size() && s[i] == '.') {
            ++i;
        } else {
            break;
        }
    }
    return n;
}

}  // namespace

const char* to_string(BootloaderClass c) noexcept {
    switch (c) {
        case BootloaderClass::Unknown: return "unknown";
        case BootloaderClass::Otafix: return "otafix";
        case BootloaderClass::UpstreamGe0_10: return "upstream_ge_0_10";
        case BootloaderClass::UpstreamLe0_9: return "upstream_le_0_9";
    }
    return "unknown";
}

uint16_t s140_fwid(std::string_view v) noexcept {
    // 6.1.1 and 7.3.0 verified from MeshCore board definitions; 7.0.1 and 7.2.0
    // from the ESPHome Adafruit DFU sd_req list.
    if (v == "6.1.1") return 0x00B6;
    if (v == "7.0.1") return 0x00CA;
    if (v == "7.2.0") return 0x0100;
    if (v == "7.3.0") return 0x0123;
    return 0;
}

uint16_t max_packet_for(BootloaderClass c) noexcept {
    switch (c) {
        case BootloaderClass::Otafix:
        case BootloaderClass::UpstreamGe0_10:
            return kPacketMax;
        case BootloaderClass::UpstreamLe0_9:
        case BootloaderClass::Unknown:
            return kPacketMaxLegacy;
    }
    return kPacketMaxLegacy;
}

BootloaderInfo classify_bootloader(std::string_view rev) {
    BootloaderInfo info;
    const auto tokens = split_ws(rev);
    if (tokens.empty()) {
        return info;
    }
    info.version = std::string(tokens[0]);
    for (size_t i = 1; i + 1 < tokens.size(); ++i) {
        if (tokens[i].size() >= 2 && tokens[i][0] == 's' && tokens[i][1] == '1') {
            info.sd_variant = std::string(tokens[i]);
            info.sd_version = std::string(tokens[i + 1]);
            if (tokens[i] == "s140") {
                info.sd_fwid = s140_fwid(tokens[i + 1]);
            }
            break;
        }
    }

    if (parse_semver(tokens[0], info.major, info.minor, info.patch) < 2) {
        info.major = info.minor = info.patch = -1;
        return info;  // Unknown
    }
    if (info.patch < 0) info.patch = 0;

    if (tokens[0].find("OTAFIX") != std::string_view::npos) {
        info.cls = BootloaderClass::Otafix;
    } else if (info.major > 0 || info.minor >= 10) {
        info.cls = BootloaderClass::UpstreamGe0_10;
    } else {
        info.cls = BootloaderClass::UpstreamLe0_9;
    }
    return info;
}

}  // namespace dfu
