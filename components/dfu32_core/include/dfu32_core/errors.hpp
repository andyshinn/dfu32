// SPDX-License-Identifier: MIT
//
// Job-level error codes for fetching, checking and staging an image
// (SPEC.md §13.3). DFU transfer errors keep their own codes in dfu_proto.
#pragma once

#include <cstdint>
#include <string>

namespace dfu32 {

enum class Err : uint8_t {
    Ok,
    HttpError,
    ManifestInvalid,
    ManifestUnsupported,
    HashMismatch,
    SizeMismatch,
    BoardMismatch,
    ImageTooLarge,
    ImageNotWordAligned,
    DatInvalid,
    DatCrcMismatch,
    DatTooLong,
    StagingIo,
    NoStagedImage,
    Aborted,
};

/// The SPEC.md §13.3 spelling, e.g. "HASH_MISMATCH".
const char* to_string(Err e) noexcept;

/// An error code plus human-readable detail.
struct Status {
    Err code = Err::Ok;
    std::string detail;

    bool ok() const noexcept { return code == Err::Ok; }
    /// "CODE: detail" (or just "CODE").
    std::string str() const;

    static Status fail(Err code, std::string detail = {}) { return {code, std::move(detail)}; }
};

}  // namespace dfu32
