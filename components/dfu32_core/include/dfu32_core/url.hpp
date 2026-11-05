// SPDX-License-Identifier: MIT
//
// http:// URL parsing and RFC 3986 reference resolution, enough for manifest
// `url` fields that are relative to the manifest URL (SPEC.md §11). v1 is
// HTTP-only (SPEC.md §2), so other schemes are refused.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dfu32 {

struct Url {
    std::string host;        // as written; IPv6 literals keep their brackets
    uint16_t port = 80;
    std::string path;        // starts with '/'
    std::string query;       // without '?', empty if none

    /// Normalised text form: http://host[:port]/path[?query]
    std::string str() const;
};

/// Parses an absolute http:// URL. Fragments are dropped. Refuses userinfo,
/// empty hosts, bad ports and any scheme other than http (error says which).
bool parse_http_url(std::string_view text, Url& out, std::string* error = nullptr);

/// Resolves `ref` against the absolute http URL `base` (RFC 3986 §5.2:
/// absolute, network-path, absolute-path, relative-path and query-only
/// references; dot segments removed). The result must itself be http.
bool resolve_url(std::string_view base, std::string_view ref, std::string& out, std::string* error = nullptr);

}  // namespace dfu32
