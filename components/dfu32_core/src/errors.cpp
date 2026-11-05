// SPDX-License-Identifier: MIT
#include "dfu32_core/errors.hpp"

namespace dfu32 {

const char* to_string(Err e) noexcept {
    switch (e) {
        case Err::Ok: return "OK";
        case Err::HttpError: return "HTTP_ERROR";
        case Err::ManifestInvalid: return "MANIFEST_INVALID";
        case Err::ManifestUnsupported: return "MANIFEST_UNSUPPORTED";
        case Err::HashMismatch: return "HASH_MISMATCH";
        case Err::SizeMismatch: return "SIZE_MISMATCH";
        case Err::BoardMismatch: return "BOARD_MISMATCH";
        case Err::ImageTooLarge: return "IMAGE_TOO_LARGE";
        case Err::ImageNotWordAligned: return "IMAGE_NOT_WORD_ALIGNED";
        case Err::DatInvalid: return "DAT_INVALID";
        case Err::DatCrcMismatch: return "DAT_CRC_MISMATCH";
        case Err::DatTooLong: return "DAT_TOO_LONG";
        case Err::StagingIo: return "STAGING_IO";
        case Err::NoStagedImage: return "NO_STAGED_IMAGE";
        case Err::Aborted: return "ABORTED";
    }
    return "UNKNOWN";
}

std::string Status::str() const {
    std::string s = to_string(code);
    if (!detail.empty()) {
        s += ": ";
        s += detail;
    }
    return s;
}

}  // namespace dfu32
