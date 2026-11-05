// SPDX-License-Identifier: MIT
#include "dfu_proto/image.hpp"

namespace dfu {

const char* to_string(ImageError e) noexcept {
    switch (e) {
        case ImageError::Ok: return "OK";
        case ImageError::Empty: return "IMAGE_EMPTY";
        case ImageError::NotWordAligned: return "IMAGE_NOT_WORD_ALIGNED";
        case ImageError::TooLarge: return "IMAGE_TOO_LARGE";
        case ImageError::WrongDeviceType: return "DAT_WRONG_DEVICE_TYPE";
        case ImageError::NoSdReq: return "DAT_NO_SD_REQ";
        case ImageError::CrcMismatch: return "DAT_CRC_MISMATCH";
    }
    return "UNKNOWN";
}

uint32_t default_max_app_size(uint16_t device_type) noexcept {
    return device_type == kNrf54DeviceType ? UINT32_MAX : kDefaultMaxAppSize;
}

ImageError check_app_image(const InitPacket& init, uint32_t bin_size, uint16_t bin_crc16,
                           uint32_t max_app_size, uint16_t expected_device_type) noexcept {
    if (bin_size == 0) {
        return ImageError::Empty;
    }
    if ((bin_size & 3u) != 0) {
        return ImageError::NotWordAligned;
    }
    if (bin_size > max_app_size) {
        return ImageError::TooLarge;
    }
    const bool known = init.device_type == kAdafruitDeviceType || init.device_type == kNrf54DeviceType;
    if (expected_device_type == kAnyKnownDeviceType ? !known : init.device_type != expected_device_type) {
        return ImageError::WrongDeviceType;
    }
    if (init.sd_req_count == 0) {
        return ImageError::NoSdReq;
    }
    if (init.firmware_crc16 != bin_crc16) {
        return ImageError::CrcMismatch;
    }
    return ImageError::Ok;
}

}  // namespace dfu
