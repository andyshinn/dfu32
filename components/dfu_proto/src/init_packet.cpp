// SPDX-License-Identifier: MIT
#include "dfu_proto/init_packet.hpp"

namespace dfu {
namespace {

uint16_t u16le(std::span<const uint8_t> d, size_t off) noexcept {
    return static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
}

}  // namespace

bool InitPacket::accepts_softdevice(uint16_t installed_fwid) const noexcept {
    for (const uint16_t req : softdevice_requirements()) {
        if (req == kSdReqAny || req == installed_fwid) {
            return true;
        }
    }
    return false;
}

const char* to_string(InitPacketError e) noexcept {
    switch (e) {
        case InitPacketError::Ok: return "OK";
        case InitPacketError::TooShort: return "TOO_SHORT";
        case InitPacketError::UnsupportedLength: return "UNSUPPORTED_LENGTH";
        case InitPacketError::NoSdReq: return "NO_SD_REQ";
        case InitPacketError::TooManySdReq: return "TOO_MANY_SD_REQ";
    }
    return "UNKNOWN";
}

InitPacketError parse_init_packet(std::span<const uint8_t> data, InitPacket& out) noexcept {
    if (data.size() < kInitPacketMinSize) {
        return InitPacketError::TooShort;
    }
    const uint16_t n = u16le(data, 8);
    if (n == 0) {
        return InitPacketError::NoSdReq;
    }
    if (n > kMaxSdReq) {
        return InitPacketError::TooManySdReq;
    }
    const size_t expected = 12 + 2 * static_cast<size_t>(n);
    if (data.size() < expected) {
        return InitPacketError::TooShort;
    }
    if (data.size() > expected) {
        return InitPacketError::UnsupportedLength;
    }

    InitPacket p;
    p.device_type = u16le(data, 0);
    p.device_rev = u16le(data, 2);
    p.app_version = static_cast<uint32_t>(u16le(data, 4)) | (static_cast<uint32_t>(u16le(data, 6)) << 16);
    p.sd_req_count = static_cast<uint8_t>(n);
    for (size_t i = 0; i < n; ++i) {
        p.sd_req[i] = u16le(data, 10 + 2 * i);
    }
    p.firmware_crc16 = u16le(data, 10 + 2 * static_cast<size_t>(n));
    out = p;
    return InitPacketError::Ok;
}

}  // namespace dfu
