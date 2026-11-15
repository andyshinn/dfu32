// SPDX-License-Identifier: MIT
#include "dfu32_core/captive.hpp"

#include <cstring>

#include "dfu32_core/ap.hpp"

namespace dfu32::captive {

namespace {

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equal_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool is_api(std::string_view path) noexcept {
    constexpr std::string_view kApi = "/api/v1";
    return path.substr(0, kApi.size()) == kApi && (path.size() == kApi.size() || path[kApi.size()] == '/');
}

}  // namespace

bool host_is_ours(std::optional<std::string_view> host_hdr, std::string_view hostname) noexcept {
    // No Host header is legal only in HTTP/1.0 (RFC 7230 §5.4), which can't be rebound
    // to a foreign origin by name; an empty value names no origin either (SPEC §9.5).
    if (!host_hdr || host_hdr->empty()) return true;
    std::string_view host = *host_hdr;
    constexpr std::string_view kPort = ":80";
    if (host.size() > kPort.size() && host.substr(host.size() - kPort.size()) == kPort) {
        host.remove_suffix(kPort.size());
    }
    if (host.size() > 1 && host.back() == '.') host.remove_suffix(1);  // FQDN form
    if (equal_ci(host, ap::kIpText)) return true;
    if (hostname.empty()) return false;
    if (equal_ci(host, hostname)) return true;
    constexpr std::string_view kLocal = ".local";
    return host.size() == hostname.size() + kLocal.size() && equal_ci(host.substr(0, hostname.size()), hostname) &&
           equal_ci(host.substr(hostname.size()), kLocal);
}

Action decide(bool on_ap, std::string_view method, std::optional<std::string_view> host, std::string_view path,
              std::string_view hostname) noexcept {
    if (const size_t q = path.find('?'); q != std::string_view::npos) path = path.substr(0, q);
    const bool api = is_api(path);
    if (!on_ap) return path == "/" || api ? Action::Serve : Action::NotFound;
    const bool ours = host_is_ours(host, hostname);
    // The captive DNS sends every name here, so a foreign page could drive the API
    // through DNS rebinding: the API answers only to our own names, and is never
    // redirected (a 302 would turn a failed POST into a GET of the UI).
    if (api) return ours ? Action::Serve : Action::Misdirected;
    // "/" from a foreign Host: redirect, so the UI always loads from one origin.
    if (path == "/") return ours ? Action::Serve : Action::Redirect;
    // The OS connectivity probes (/hotspot-detect.html, /generate_204, ...) must
    // not get their "online" answer, so the phone shows its sign-in sheet.
    if (method == "GET" || method == "HEAD") return Action::Redirect;
    return Action::NotFound;
}

size_t dns_answer(const uint8_t* query, size_t len, const uint8_t ip[4], uint8_t* out, size_t cap) noexcept {
    constexpr size_t kHeader = 12;
    constexpr size_t kAnswer = 16;     // ptr(2) type(2) class(2) ttl(4) rdlen(2) addr(4)
    constexpr size_t kMaxName = 255;   // RFC 1035 §2.3.4, wire form including the root label
    if (query == nullptr || ip == nullptr || out == nullptr) return 0;
    if (len < kHeader || len > kDnsMaxPacket) return 0;
    if (query[2] & 0x80) return 0;                 // a response, not a query
    if (((query[2] >> 3) & 0x0F) != 0) return 0;   // opcode: QUERY only
    if (query[4] != 0 || query[5] != 1) return 0;  // exactly one question

    // QNAME: labels up to the root. A pointer (or the reserved 0x40/0x80 forms) is
    // refused: a query has nothing earlier to point at.
    size_t p = kHeader;
    size_t name = 0;
    for (;;) {
        if (p >= len) return 0;
        const uint8_t l = query[p++];
        if (l & 0xC0) return 0;  // also covers labels over 63
        name += size_t{l} + 1;
        if (name > kMaxName) return 0;
        if (l == 0) break;
        if (l > len - p) return 0;
        p += l;
    }
    if (len - p < 4) return 0;  // QTYPE, QCLASS
    const uint16_t qtype = static_cast<uint16_t>(query[p] << 8 | query[p + 1]);
    const uint16_t qclass = static_cast<uint16_t>(query[p + 2] << 8 | query[p + 3]);
    p += 4;

    // Every other type (AAAA, HTTPS 65, SVCB 64, ...) gets a quick NOERROR with no
    // answer: iOS asks for HTTPS records first and waits on a missing reply.
    const bool answer = qtype == 1 && qclass == 1;
    const size_t total = p + (answer ? kAnswer : 0);
    if (cap < total) return 0;

    std::memmove(out, query, p);  // header and question, case kept (0x20 randomisation)
    out[2] = static_cast<uint8_t>(0x84 | (query[2] & 0x01));  // QR, AA, opcode 0, RD copied
    out[3] = 0;                                               // RA 0, NOERROR
    out[6] = 0;
    out[7] = answer ? 1 : 0;
    std::memset(out + 8, 0, 4);  // NSCOUNT, ARCOUNT: an EDNS OPT record isn't echoed
    if (answer) {
        uint8_t* a = out + p;
        const uint8_t rr[kAnswer] = {
            0xC0, 0x0C,  // name: pointer to the question
            0x00, 0x01, 0x00, 0x01,  // A, IN
            static_cast<uint8_t>(kDnsTtlS >> 24), static_cast<uint8_t>(kDnsTtlS >> 16),
            static_cast<uint8_t>(kDnsTtlS >> 8), static_cast<uint8_t>(kDnsTtlS),  // nothing stale survives the AP
            0x00, 0x04, ip[0], ip[1], ip[2], ip[3],
        };
        std::memcpy(a, rr, kAnswer);
    }
    return total;
}

}  // namespace dfu32::captive
