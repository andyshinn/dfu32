// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "dfu32_core/ap.hpp"
#include "dfu32_core/captive.hpp"

using namespace dfu32::captive;
namespace ap = dfu32::ap;

namespace {

constexpr const char* kHostname = "dfu32-ab12";

using Bytes = std::vector<uint8_t>;

// A query with id 0xBEEF and RD set: header, then labels from `name`, then type/class.
Bytes query(const std::string& name, uint16_t qtype, uint16_t qclass = 1, uint16_t flags = 0x0100,
            uint16_t qdcount = 1, uint16_t arcount = 0) {
    Bytes q = {0xBE, 0xEF, static_cast<uint8_t>(flags >> 8), static_cast<uint8_t>(flags), 0,
               static_cast<uint8_t>(qdcount), 0, 0, 0, 0, 0, static_cast<uint8_t>(arcount)};
    size_t start = 0;
    while (start < name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        q.push_back(static_cast<uint8_t>(dot - start));
        q.insert(q.end(), name.begin() + static_cast<long>(start), name.begin() + static_cast<long>(dot));
        start = dot + 1;
    }
    q.push_back(0);
    q.push_back(static_cast<uint8_t>(qtype >> 8));
    q.push_back(static_cast<uint8_t>(qtype));
    q.push_back(static_cast<uint8_t>(qclass >> 8));
    q.push_back(static_cast<uint8_t>(qclass));
    return q;
}

size_t answer(const Bytes& q, Bytes& out, size_t cap = kDnsMaxPacket) {
    out.assign(kDnsMaxPacket + 64, 0xAA);
    return dns_answer(q.data(), q.size(), ap::kIp, out.data(), cap);
}

}  // namespace

TEST_CASE("Captive: our host names", "[captive]") {
    for (const char* h : {"192.168.4.1", "192.168.4.1:80", "DFU32-AB12.local", "dfu32-ab12", "dfu32-ab12.local.",
                          "dfu32-ab12:80", "192.168.4.1.", ""}) {
        INFO(h);
        CHECK(host_is_ours(h, kHostname));
    }
    for (const char* h : {"captive.apple.com", "192.168.4.1:8080", "192.168.4.10", "dfu32-ab12.lan",
                          "dfu32-ab12.local.evil", "x.dfu32-ab12.local", ".local", ":80", "dfu32-ab12..", "."}) {
        INFO(h);
        CHECK_FALSE(host_is_ours(h, kHostname));
    }
    CHECK_FALSE(host_is_ours(".local", ""));
    CHECK(host_is_ours("192.168.4.1", ""));
}

TEST_CASE("Captive: HTTP decision", "[captive]") {
    SECTION("off the AP nothing changes") {
        for (const char* host : {"captive.apple.com", "192.168.1.50", ""}) {
            CHECK(decide(false, "GET", host, "/", kHostname) == Action::Serve);
            CHECK(decide(false, "GET", host, "/api/v1/status", kHostname) == Action::Serve);
            CHECK(decide(false, "POST", host, "/api/v1/claim", kHostname) == Action::Serve);
            CHECK(decide(false, "GET", host, "/generate_204", kHostname) == Action::NotFound);
            CHECK(decide(false, "HEAD", host, "/hotspot-detect.html", kHostname) == Action::NotFound);
            CHECK(decide(false, "POST", host, "/nope", kHostname) == Action::NotFound);
        }
    }
    SECTION("on the AP the OS probes are redirected") {
        for (const char* path : {"/hotspot-detect.html", "/generate_204", "/gen_204", "/connecttest.txt", "/ncsi.txt",
                                 "/redirect", "/canonical.html", "/success.txt"}) {
            for (const char* method : {"GET", "HEAD"}) {
                INFO(method << " " << path);
                CHECK(decide(true, method, "captive.apple.com", path, kHostname) == Action::Redirect);
                CHECK(decide(true, method, "192.168.4.1", path, kHostname) == Action::Redirect);
            }
        }
        CHECK(decide(true, "GET", "connectivitycheck.gstatic.com", "/", kHostname) == Action::Redirect);
        CHECK(decide(true, "POST", "192.168.4.1", "/nope", kHostname) == Action::NotFound);
        CHECK(decide(true, "GET", "192.168.4.1", "/", kHostname) == Action::Serve);
        CHECK(decide(true, "GET", "DFU32-AB12.local", "/", kHostname) == Action::Serve);
    }
    SECTION("the API answers only to our names and is never redirected") {
        CHECK(decide(true, "GET", "evil.example", "/api/v1/status", kHostname) == Action::Misdirected);
        CHECK(decide(true, "POST", "evil.example", "/api/v1/targets/x/flash", kHostname) == Action::Misdirected);
        CHECK(decide(true, "GET", "evil.example", "/api/v1", kHostname) == Action::Misdirected);
        CHECK(decide(true, "GET", "192.168.4.1", "/api/v1/status", kHostname) == Action::Serve);
        CHECK(decide(true, "GET", "", "/api/v1/status", kHostname) == Action::Serve);
        CHECK(decide(true, "GET", "evil.example", "/api/v1x", kHostname) == Action::Redirect);  // not the API
    }
    SECTION("the query is ignored") {
        CHECK(decide(true, "GET", "evil.example", "/?a=/api/v1/status", kHostname) == Action::Redirect);
        CHECK(decide(true, "GET", "192.168.4.1", "/?x=1", kHostname) == Action::Serve);
        CHECK(decide(true, "GET", "evil.example", "/api/v1/logs?since=1", kHostname) == Action::Misdirected);
        CHECK(decide(false, "GET", "evil.example", "/?x", kHostname) == Action::Serve);
    }
}

TEST_CASE("Captive: DNS A answer bytes", "[captive]") {
    const Bytes q = query("captive.apple.com", 1);
    Bytes out;
    const size_t n = answer(q, out);
    REQUIRE(n == q.size() + 16);
    Bytes expect = q;
    expect[2] = 0x85;  // QR, AA, RD
    expect[3] = 0x00;
    expect[7] = 1;     // ANCOUNT
    for (uint8_t b : {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xC0, 0xA8, 0x04, 0x01}) {
        expect.push_back(b);
    }
    CHECK(Bytes(out.begin(), out.begin() + static_cast<long>(n)) == expect);
    CHECK(out[n] == 0xAA);  // nothing written past the reply

    // RD clear stays clear; the question's case is echoed as sent.
    const Bytes q2 = query("CaPtIvE.apple.com", 1, 1, 0x0000);
    REQUIRE(answer(q2, out) == q2.size() + 16);
    CHECK(out[2] == 0x84);
    CHECK(Bytes(out.begin() + 12, out.begin() + static_cast<long>(q2.size())) ==
          Bytes(q2.begin() + 12, q2.end()));
}

TEST_CASE("Captive: DNS negative answers", "[captive]") {
    Bytes out;
    for (uint16_t type : {28, 65, 64, 16}) {  // AAAA, HTTPS, SVCB, TXT
        INFO(type);
        const Bytes q = query("captive.apple.com", type);
        REQUIRE(answer(q, out) == q.size());
        CHECK(out[3] == 0);  // NOERROR
        CHECK(out[6] == 0);
        CHECK(out[7] == 0);
    }
    const Bytes ch = query("version.bind", 16, 3);  // class CH
    REQUIRE(answer(ch, out) == ch.size());
    CHECK(out[7] == 0);
    const Bytes chA = query("version.bind", 1, 3);
    REQUIRE(answer(chA, out) == chA.size());
    CHECK(out[7] == 0);

    // EDNS: the OPT record after the question is not echoed.
    Bytes edns = query("example.com", 1, 1, 0x0100, 1, 1);
    const size_t qlen = edns.size();
    for (uint8_t b : {0x00, 0x00, 0x29, 0x04, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}) edns.push_back(b);
    REQUIRE(answer(edns, out) == qlen + 16);
    CHECK(out[10] == 0);
    CHECK(out[11] == 0);

    const Bytes long_label = query(std::string(63, 'a') + ".com", 1);
    CHECK(answer(long_label, out) == long_label.size() + 16);
}

TEST_CASE("Captive: DNS drops", "[captive]") {
    Bytes out;
    const Bytes good = query("example.com", 1);
    CHECK(answer(query("example.com", 1, 1, 0x8100), out) == 0);  // QR set
    CHECK(answer(query("example.com", 1, 1, 0x1000), out) == 0);  // opcode 2 (STATUS)
    CHECK(answer(query("example.com", 1, 1, 0x0100, 0), out) == 0);
    CHECK(answer(query("example.com", 1, 1, 0x0100, 2), out) == 0);
    CHECK(answer(Bytes(good.begin(), good.begin() + 11), out) == 0);
    CHECK(answer(Bytes(good.begin(), good.begin() + 14), out) == 0);  // a label past len
    CHECK(answer(Bytes(good.end() - 2, good.end()), out) == 0);
    CHECK(answer(Bytes(good.begin(), good.end() - 1), out) == 0);    // truncated QCLASS

    Bytes ptr = query("", 1);
    ptr[12] = 0xC0;
    ptr.insert(ptr.begin() + 13, 0x0C);
    CHECK(answer(ptr, out) == 0);
    CHECK(answer(query(std::string(64, 'a') + ".com", 1), out) == 0);  // label > 63

    // 4 x 63-byte labels = 256 bytes of name with the root: over 255.
    const std::string l63(63, 'a');
    CHECK(answer(query(l63 + "." + l63 + "." + l63 + "." + l63, 1), out) == 0);
    // 253 characters of dotted name (255 on the wire) is the most there can be.
    const std::string max = l63 + "." + l63 + "." + l63 + "." + std::string(61, 'b');
    REQUIRE(max.size() == 253);
    CHECK(answer(query(max, 1), out) == query(max, 1).size() + 16);

    CHECK(answer(good, out, good.size() + 15) == 0);  // cap one byte short
    CHECK(out[0] == 0xAA);                             // and nothing written
    CHECK(answer(good, out, good.size() + 16) == good.size() + 16);
    const Bytes aaaa = query("example.com", 28);
    CHECK(answer(aaaa, out, aaaa.size() - 1) == 0);

    Bytes huge = good;
    huge.resize(kDnsMaxPacket + 1, 0);
    CHECK(answer(huge, out) == 0);
    CHECK(dns_answer(nullptr, 12, ap::kIp, out.data(), out.size()) == 0);
}

TEST_CASE("Captive: DNS fuzz", "[captive]") {
    std::mt19937 rng(0xCA971E);
    const auto byte = [&] { return static_cast<uint8_t>(rng() & 0xFF); };
    Bytes out(kDnsMaxPacket + 64);
    const auto check = [&](const Bytes& in) {
        const size_t cap = rng() % (kDnsMaxPacket + 1);
        std::fill(out.begin(), out.end(), 0x5A);
        const size_t n = dns_answer(in.data(), in.size(), ap::kIp, out.data(), cap);
        REQUIRE(n <= cap);
        for (size_t i = cap; i < out.size(); ++i) REQUIRE(out[i] == 0x5A);
        if (n != 0) {
            REQUIRE(in.size() >= 12);
            REQUIRE(out[0] == in[0]);
            REQUIRE(out[1] == in[1]);
            REQUIRE((out[2] & 0x80) != 0);
        }
    };

    for (int i = 0; i < 50000; ++i) {
        Bytes in(rng() % 80);
        for (auto& b : in) b = byte();
        if (in.size() >= 6 && (rng() & 1)) {  // make many of them look like one query
            in[2] &= 0x07;
            in[4] = 0;
            in[5] = 1;
        }
        check(in);
    }
    const Bytes seeds[] = {query("captive.apple.com", 1), query("example.com", 65, 1, 0x0100, 1, 1),
                           query(std::string(63, 'z') + ".local", 28)};
    for (int i = 0; i < 50000; ++i) {
        Bytes in = seeds[static_cast<size_t>(i) % 3];
        in[rng() % in.size()] = byte();
        check(in);
    }
}
