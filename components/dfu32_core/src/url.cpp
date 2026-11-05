// SPDX-License-Identifier: MIT
#include "dfu32_core/url.hpp"

#include <cctype>
#include <vector>

namespace dfu32 {
namespace {

bool set_err(std::string* error, const char* msg) {
    if (error != nullptr) *error = msg;
    return false;
}

std::string_view strip_fragment(std::string_view s) {
    const size_t hash = s.find('#');
    return hash == std::string_view::npos ? s : s.substr(0, hash);
}

// The scheme ("http") if `s` starts with one, else empty.
std::string_view scheme_of(std::string_view s) {
    if (s.empty() || !std::isalpha(static_cast<unsigned char>(s[0]))) return {};
    for (size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        if (c == ':') return s.substr(0, i);
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') return {};
    }
    return {};
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// RFC 3986 §5.2.4.
std::string remove_dot_segments(std::string_view path) {
    std::vector<std::string_view> out;
    const bool absolute = !path.empty() && path[0] == '/';
    size_t i = absolute ? 1 : 0;
    bool trailing_slash = false;
    while (i <= path.size()) {
        const size_t slash = path.find('/', i);
        const std::string_view seg = path.substr(i, slash == std::string_view::npos ? std::string_view::npos : slash - i);
        const bool last = slash == std::string_view::npos;
        if (seg == ".") {
            trailing_slash = last;
        } else if (seg == "..") {
            if (!out.empty()) out.pop_back();
            trailing_slash = last;
        } else {
            out.push_back(seg);
            trailing_slash = false;
        }
        if (last) break;
        i = slash + 1;
    }
    std::string r = absolute ? "/" : "";
    for (size_t k = 0; k < out.size(); ++k) {
        if (k) r += '/';
        r += out[k];
    }
    if (trailing_slash && (r.empty() || r.back() != '/')) r += '/';
    return r;
}

}  // namespace

std::string Url::str() const {
    std::string s = "http://" + host;
    if (port != 80) s += ":" + std::to_string(port);
    s += path;
    if (!query.empty()) s += "?" + query;
    return s;
}

bool parse_http_url(std::string_view text, Url& out, std::string* error) {
    text = strip_fragment(text);
    const std::string_view scheme = scheme_of(text);
    if (scheme.empty()) return set_err(error, "not an absolute URL");
    if (!iequals(scheme, "http")) {
        return set_err(error, iequals(scheme, "https") ? "https is not supported (v1 is HTTP only)"
                                                       : "unsupported URL scheme (want http://)");
    }
    std::string_view rest = text.substr(scheme.size() + 1);
    if (rest.substr(0, 2) != "//") return set_err(error, "missing '//' after http:");
    rest.remove_prefix(2);
    const size_t auth_end = rest.find_first_of("/?");
    const std::string_view authority = rest.substr(0, auth_end);
    rest = auth_end == std::string_view::npos ? std::string_view{} : rest.substr(auth_end);
    if (authority.find('@') != std::string_view::npos) return set_err(error, "credentials in URLs are not supported");

    std::string_view host = authority;
    std::string_view port;
    if (!host.empty() && host[0] == '[') {
        const size_t close = host.find(']');
        if (close == std::string_view::npos) return set_err(error, "unterminated IPv6 literal");
        if (close + 1 < host.size()) {
            if (host[close + 1] != ':') return set_err(error, "bad host");
            port = host.substr(close + 2);
        }
        host = host.substr(0, close + 1);
    } else if (const size_t colon = host.rfind(':'); colon != std::string_view::npos) {
        port = host.substr(colon + 1);
        host = host.substr(0, colon);
    }
    if (host.empty() || host == "[]") return set_err(error, "empty host");
    for (const char c : host) {
        if (std::isspace(static_cast<unsigned char>(c))) return set_err(error, "bad host");
    }
    uint32_t port_num = 80;
    if (!port.empty() || authority.back() == ':') {
        if (port.empty() || port.size() > 5) return set_err(error, "bad port");
        port_num = 0;
        for (const char c : port) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return set_err(error, "bad port");
            port_num = port_num * 10 + static_cast<uint32_t>(c - '0');
        }
        if (port_num == 0 || port_num > 65535) return set_err(error, "bad port");
    }

    Url u;
    u.host.assign(host);
    u.port = static_cast<uint16_t>(port_num);
    const size_t q = rest.find('?');
    const std::string_view path = rest.substr(0, q);
    u.path = path.empty() ? "/" : remove_dot_segments(path);
    if (q != std::string_view::npos) u.query.assign(rest.substr(q + 1));
    for (const char c : u.path + u.query) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) return set_err(error, "spaces or control characters in URL");
    }
    out = std::move(u);
    return true;
}

bool resolve_url(std::string_view base, std::string_view ref, std::string& out, std::string* error) {
    Url b;
    if (!parse_http_url(base, b, error)) return false;
    ref = strip_fragment(ref);

    if (!scheme_of(ref).empty()) {
        Url r;
        if (!parse_http_url(ref, r, error)) return false;
        out = r.str();
        return true;
    }
    std::string candidate;
    if (ref.substr(0, 2) == "//") {
        candidate = "http:" + std::string(ref);
    } else {
        Url r = b;
        if (ref.empty()) {
            // Same document.
        } else if (ref[0] == '?') {
            r.query.assign(ref.substr(1));
        } else {
            const size_t q = ref.find('?');
            const std::string_view rpath = ref.substr(0, q);
            r.query = q == std::string_view::npos ? std::string{} : std::string(ref.substr(q + 1));
            if (rpath.empty()) {
                // "?" handled above; an empty path with a query keeps the base path.
            } else if (rpath[0] == '/') {
                r.path = remove_dot_segments(rpath);
            } else {
                const std::string merged = b.path.substr(0, b.path.rfind('/') + 1) + std::string(rpath);
                r.path = remove_dot_segments(merged);
            }
        }
        candidate = r.str();
    }
    Url final_url;
    if (!parse_http_url(candidate, final_url, error)) return false;
    out = final_url.str();
    return true;
}

}  // namespace dfu32
