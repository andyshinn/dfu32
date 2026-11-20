// SPDX-License-Identifier: MIT
//
// Captive portal for DFU32's access point (SPEC.md §9.5): the HTTP decision
// (serve, redirect to http://192.168.4.1/, or refuse a foreign Host on the API)
// and the DNS answer that resolves every name to the AP. Pure, so both are
// host-tested and the DNS parser is fuzzed; the httpd and lwIP glue live in
// components/api and components/net.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dfu32::captive {

enum class Action : uint8_t { Serve, Redirect, Misdirected, NotFound };

/// AP IP, <hostname>, <hostname>.local; case-insensitive; optional ":80"; one trailing '.'.
/// nullopt (no Host header, HTTP/1.0) and an empty value are ours. The glue fails closed on a Host it
/// could not read (e.g. too long for its buffer) by passing a foreign value, never nullopt (§9.5).
bool host_is_ours(std::optional<std::string_view> host, std::string_view hostname) noexcept;

/// path may carry "?query" (ignored). method: "GET","HEAD","POST",...
/// !on_ap: "/" and "/api/v1/..." -> Serve, others -> NotFound (LAN unchanged).
/// on_ap:  "/api/v1" or "/api/v1/..." -> ours ? Serve : Misdirected (never Redirect);
///         "/" -> ours ? Serve : Redirect; other paths -> GET/HEAD ? Redirect : NotFound.
Action decide(bool on_ap, std::string_view method, std::optional<std::string_view> host, std::string_view path,
              std::string_view hostname) noexcept;

inline constexpr size_t kDnsMaxPacket = 512;
inline constexpr uint32_t kDnsTtlS = 0;

/// Captive DNS reply for one UDP query. A/IN -> one answer (name ptr 0xC00C, type 1, class 1, TTL kDnsTtlS,
/// rdlen 4, ip). Any other qtype/qclass -> NOERROR, ANCOUNT 0 (question echoed). Flags: QR=1, AA=1, opcode 0,
/// RD copied, RA=0. NSCOUNT=ARCOUNT=0 (EDNS OPT not echoed). Returns reply length; 0 = drop: len<12,
/// len>kDnsMaxPacket, QR set, opcode!=0, QDCOUNT!=1, label>63, name>255, compression pointer in qname,
/// truncated question, cap too small.
size_t dns_answer(const uint8_t* query, size_t len, const uint8_t ip[4], uint8_t* out, size_t cap) noexcept;

}  // namespace dfu32::captive
