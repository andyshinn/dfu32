// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "dfu32_core/url.hpp"

using dfu32::parse_http_url;
using dfu32::resolve_url;
using dfu32::Url;

TEST_CASE("URL: parse http URLs", "[url]") {
    Url u;
    REQUIRE(parse_http_url("http://fw.lan/meshcore/rak/manifest.json", u));
    CHECK(u.host == "fw.lan");
    CHECK(u.port == 80);
    CHECK(u.path == "/meshcore/rak/manifest.json");
    CHECK(u.str() == "http://fw.lan/meshcore/rak/manifest.json");

    REQUIRE(parse_http_url("HTTP://192.168.1.5:8080?x=1#frag", u));
    CHECK(u.host == "192.168.1.5");
    CHECK(u.port == 8080);
    CHECK(u.path == "/");
    CHECK(u.query == "x=1");
    CHECK(u.str() == "http://192.168.1.5:8080/?x=1");

    REQUIRE(parse_http_url("http://[fd00::1]:81/a", u));
    CHECK(u.host == "[fd00::1]");
    CHECK(u.port == 81);
    REQUIRE(parse_http_url("http://h:80/a/./b/../c", u));
    CHECK(u.str() == "http://h/a/c");
}

TEST_CASE("URL: refuse what v1 doesn't support", "[url]") {
    Url u;
    std::string err;
    CHECK_FALSE(parse_http_url("https://fw.lan/m.json", u, &err));
    CHECK(err.find("https") != std::string::npos);
    CHECK_FALSE(parse_http_url("ftp://fw.lan/m.json", u, &err));
    CHECK_FALSE(parse_http_url("fw.lan/m.json", u, &err));
    CHECK_FALSE(parse_http_url("http:/fw.lan", u, &err));
    CHECK_FALSE(parse_http_url("http://user:pw@fw.lan/", u, &err));
    CHECK_FALSE(parse_http_url("http:///path", u, &err));
    CHECK_FALSE(parse_http_url("http://h:0/", u, &err));
    CHECK_FALSE(parse_http_url("http://h:65536/", u, &err));
    CHECK_FALSE(parse_http_url("http://h:/", u, &err));
    CHECK_FALSE(parse_http_url("http://h:8a/", u, &err));
    CHECK_FALSE(parse_http_url("http://h/a b", u, &err));
    CHECK_FALSE(parse_http_url("http://[fd00::1/", u, &err));
    CHECK_FALSE(parse_http_url("http://fe80::1/manifest.json", u, &err));  // unbracketed IPv6
    CHECK(err.find("brackets") != std::string::npos);
}

TEST_CASE("URL: resolve manifest-relative references (RFC 3986 §5.4)", "[url]") {
    const char* base = "http://fw.lan:8000/meshcore/rak4631/manifest.json?ch=beta";
    std::string out;
    auto r = [&](const char* ref) {
        out.clear();
        REQUIRE(resolve_url(base, ref, out));
        return out;
    };
    CHECK(r("firmware.bin") == "http://fw.lan:8000/meshcore/rak4631/firmware.bin");
    CHECK(r("./firmware.bin") == "http://fw.lan:8000/meshcore/rak4631/firmware.bin");
    CHECK(r("../common/x.dat") == "http://fw.lan:8000/meshcore/common/x.dat");
    CHECK(r("../../../../x") == "http://fw.lan:8000/x");
    CHECK(r("/abs/f.bin") == "http://fw.lan:8000/abs/f.bin");
    CHECK(r("//cdn.lan/f.bin") == "http://cdn.lan/f.bin");
    CHECK(r("http://other:9/f.bin") == "http://other:9/f.bin");
    CHECK(r("f.bin?sig=1") == "http://fw.lan:8000/meshcore/rak4631/f.bin?sig=1");
    CHECK(r("?ch=stable") == "http://fw.lan:8000/meshcore/rak4631/manifest.json?ch=stable");
    CHECK(r("") == "http://fw.lan:8000/meshcore/rak4631/manifest.json?ch=beta");
    CHECK(r("sub/") == "http://fw.lan:8000/meshcore/rak4631/sub/");
    CHECK(r("f.bin#frag") == "http://fw.lan:8000/meshcore/rak4631/f.bin");

    std::string err;
    CHECK_FALSE(resolve_url(base, "https://fw.lan/f.bin", out, &err));
    CHECK_FALSE(resolve_url("not a url", "f.bin", out, &err));
    CHECK_FALSE(resolve_url(base, "mailto:x@y", out, &err));
}
