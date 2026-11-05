// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "dfu32_core/manifest.hpp"
#include "sha256_ref.hpp"

TEST_CASE("Reference SHA-256 matches FIPS 180-4 vectors", "[sha256]") {
    CHECK(dfu32::sha256_hex(RefSha256::of("")) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(dfu32::sha256_hex(RefSha256::of("abc")) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(dfu32::sha256_hex(RefSha256::of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string million(1000000, 'a');
    CHECK(dfu32::sha256_hex(RefSha256::of(million)) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 hex round trip", "[sha256]") {
    const auto d = RefSha256::of("abc");
    dfu32::Sha256 back{};
    REQUIRE(dfu32::parse_sha256_hex(dfu32::sha256_hex(d), back));
    CHECK(back == d);
    CHECK(dfu32::parse_sha256_hex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", back));
    CHECK(back == d);
    CHECK_FALSE(dfu32::parse_sha256_hex("ba78", back));
    CHECK_FALSE(dfu32::parse_sha256_hex(std::string(63, 'a') + "g", back));
}
