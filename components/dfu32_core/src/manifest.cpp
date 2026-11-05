// SPDX-License-Identifier: MIT
#include "dfu32_core/manifest.hpp"

#include <algorithm>
#include <cctype>

#include "dfu32_core/json.hpp"
#include "dfu32_core/url.hpp"
#include "dfu_proto/staging_format.hpp"

namespace dfu32 {
namespace {

Status invalid(std::string detail) { return Status::fail(Err::ManifestInvalid, std::move(detail)); }

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// An optional string member: absent is fine, any other type is not.
bool opt_string(const json::Value& obj, const char* key, std::string& out) {
    const json::Value* v = obj.find(key);
    if (v == nullptr || v->is_null()) return true;
    if (!v->is_string()) return false;
    out = v->as_string();
    return true;
}

bool get_u16(const json::Value& v, uint16_t& out) {
    if (!v.is_integer() || v.as_int() < 0 || v.as_int() > 0xFFFF) return false;
    out = static_cast<uint16_t>(v.as_int());
    return true;
}

Status parse_file(const json::Value& root, const char* key, std::string_view manifest_url, ManifestFile& out) {
    const std::string k = key;
    const json::Value* f = root.find(key);
    if (f == nullptr || !f->is_object()) return invalid("'" + k + "' must be an object");
    const json::Value* url = f->find("url");
    if (url == nullptr || !url->is_string() || url->as_string().empty()) {
        return invalid("'" + k + ".url' must be a non-empty string");
    }
    std::string why;
    if (!resolve_url(manifest_url, url->as_string(), out.url, &why)) {
        return invalid("'" + k + ".url': " + why);
    }
    if (out.url.size() > kMaxUrlLen) return invalid("'" + k + ".url' resolves to more than 255 characters");
    const json::Value* size = f->find("size");
    if (size == nullptr || !size->is_integer() || size->as_int() <= 0 || size->as_int() > 0xFFFFFFFF) {
        return invalid("'" + k + ".size' must be a positive integer");
    }
    out.size = static_cast<uint32_t>(size->as_int());
    const json::Value* sha = f->find("sha256");
    if (sha == nullptr || !sha->is_string() || !parse_sha256_hex(sha->as_string(), out.sha256)) {
        return invalid("'" + k + ".sha256' must be 64 hex digits");
    }
    return {};
}

}  // namespace

bool parse_sha256_hex(std::string_view hex, Sha256& out) noexcept {
    if (hex.size() != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        const int hi = hexval(hex[2 * i]);
        const int lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::string sha256_hex(std::span<const uint8_t, 32> digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        s[2 * i] = kHex[digest[i] >> 4];
        s[2 * i + 1] = kHex[digest[i] & 0x0F];
    }
    return s;
}

Status parse_manifest(std::string_view text, std::string_view manifest_url, Manifest& out) {
    out = Manifest{};
    if (text.size() > kMaxManifestBytes) return invalid("larger than 8 KB");
    json::Value root;
    std::string why;
    if (!json::parse(text, root, &why)) return invalid("not JSON: " + why);
    if (!root.is_object()) return invalid("top level must be an object");

    const json::Value* schema = root.find("schema");
    if (schema == nullptr || !schema->is_integer() || schema->as_int() < 1) {
        return invalid("'schema' must be a positive integer");
    }
    if (schema->as_int() > kManifestSchema) {
        return Status::fail(Err::ManifestUnsupported,
                            "schema " + std::to_string(schema->as_int()) + " (this DFU32 understands 1)");
    }
    out.schema = static_cast<uint32_t>(schema->as_int());

    const json::Value* version = root.find("version");
    if (version == nullptr || !version->is_string() || version->as_string().empty()) {
        return invalid("'version' must be a non-empty string");
    }
    if (version->as_string().size() > kMaxVersionLen) return invalid("'version' longer than 63 characters");
    out.version = version->as_string();

    const json::Value* boards = root.find("boards");
    if (boards == nullptr || !boards->is_array() || boards->size() == 0) {
        return invalid("'boards' must be a non-empty array");
    }
    for (const auto& b : boards->items()) {
        if (!b.is_string() || b.as_string().empty() || b.as_string().size() > kMaxBoardLen) {
            return invalid("'boards' entries must be strings of 1-31 characters");
        }
        out.boards.push_back(b.as_string());
    }

    if (!opt_string(root, "name", out.name) || !opt_string(root, "created", out.created) ||
        !opt_string(root, "notes", out.notes)) {
        return invalid("'name', 'created' and 'notes' must be strings");
    }

    if (Status s = parse_file(root, "bin", manifest_url, out.bin); !s.ok()) return s;
    if (Status s = parse_file(root, "dat", manifest_url, out.dat); !s.ok()) return s;
    if (out.dat.size > dfu::kStagingMaxDat) {
        return Status::fail(Err::DatTooLong, "'dat.size' " + std::to_string(out.dat.size) + " > 64 bytes");
    }

    if (const json::Value* init = root.find("init"); init != nullptr && !init->is_null()) {
        if (!init->is_object()) return invalid("'init' must be an object");
        out.init.present = true;
        if (const json::Value* dt = init->find("device_type"); dt != nullptr) {
            if (!get_u16(*dt, out.init.device_type)) return invalid("'init.device_type' must be 0-65535");
            out.init.has_device_type = true;
        }
        if (const json::Value* sd = init->find("sd_req"); sd != nullptr) {
            if (!sd->is_array() || sd->size() == 0 || sd->size() > dfu::kMaxSdReq) {
                return invalid("'init.sd_req' must be an array of 1-16 integers");
            }
            for (const auto& item : sd->items()) {
                uint16_t v = 0;
                if (!get_u16(item, v)) return invalid("'init.sd_req' entries must be 0-65535");
                out.init.sd_req.push_back(v);
            }
            out.init.has_sd_req = true;
        }
        if (const json::Value* crc = init->find("crc16"); crc != nullptr) {
            if (!get_u16(*crc, out.init.crc16)) return invalid("'init.crc16' must be 0-65535");
            out.init.has_crc16 = true;
        }
    }
    return {};
}

bool board_matches(const Manifest& m, std::string_view board) noexcept {
    return std::any_of(m.boards.begin(), m.boards.end(), [&](const std::string& b) {
        return b.size() == board.size() && std::equal(b.begin(), b.end(), board.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    });
}

Status check_init_against_manifest(const Manifest& m, const dfu::InitPacket& init) {
    const ManifestInit& mi = m.init;
    if (!mi.present) return {};
    if (mi.has_device_type && mi.device_type != init.device_type) {
        return Status::fail(Err::DatInvalid, "manifest init.device_type " + std::to_string(mi.device_type) +
                                                 " != .dat " + std::to_string(init.device_type));
    }
    if (mi.has_sd_req) {
        const auto dat_sd = init.softdevice_requirements();
        if (!std::equal(mi.sd_req.begin(), mi.sd_req.end(), dat_sd.begin(), dat_sd.end())) {
            return Status::fail(Err::DatInvalid, "manifest init.sd_req differs from the .dat");
        }
    }
    if (mi.has_crc16 && mi.crc16 != init.firmware_crc16) {
        return Status::fail(Err::DatInvalid, "manifest init.crc16 " + std::to_string(mi.crc16) + " != .dat " +
                                                 std::to_string(init.firmware_crc16));
    }
    return {};
}

}  // namespace dfu32
