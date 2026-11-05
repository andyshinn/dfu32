// SPDX-License-Identifier: MIT
//
// The dfu32::json wrapper over cJSON: ownership, case-sensitive lookup,
// exact-integer reads, whole-document parsing, the nesting limit, and the
// writer. JSON grammar itself is cJSON's job; these pin the behaviour the
// manifest and config code relies on.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "dfu32_core/json.hpp"

using dfu32::json::Doc;
using dfu32::json::Node;
using dfu32::json::ObjectWriter;

TEST_CASE("JSON: objects, arrays and scalars", "[json]") {
    Doc d;
    std::string err;
    REQUIRE(d.parse(R"( {"a": 1, "b": [true, false, null], "c": "x\ty", "d": {"e": -42}} )", &err));
    const Node r = d.root();
    REQUIRE(r.is_object());
    CHECK(r.size() == 4);
    int64_t i = 0;
    CHECK(r.get("a").integer(i));
    CHECK(i == 1);
    const Node b = r.get("b");
    REQUIRE(b.is_array());
    const auto items = b.items();
    REQUIRE(items.size() == 3);
    CHECK(items[0].boolean() == true);
    CHECK(items[1].is_bool());
    CHECK(items[2].is_null());
    CHECK(r.get("c").str() == "x\ty");
    CHECK(r.get("d").get("e").integer(i));
    CHECK(i == -42);
    CHECK(r.get("missing").missing());
    CHECK(r.get("missing").get("deeper").missing());  // chaining through missing is safe
    CHECK(r.get("a").str().empty());                   // wrong type reads as empty
    CHECK(r.get("c").boolean(true) == true);           // fallback for non-bools
}

TEST_CASE("JSON: member lookup is case-sensitive", "[json]") {
    Doc d;
    REQUIRE(d.parse(R"({"Version":"x","version":"y"})"));
    CHECK(d.root().get("version").str() == "y");
    CHECK(d.root().get("Version").str() == "x");
    CHECK(d.root().get("VERSION").missing());
}

TEST_CASE("JSON: integers are exact or refused", "[json]") {
    Doc d;
    int64_t i = 0;
    REQUIRE(d.parse("[0, 802816, -1, 9007199254740992, 9007199254740994, 1.5, 1e3, 4294967295]"));
    const auto v = d.root().items();
    CHECK(v[0].integer(i));
    CHECK(i == 0);
    CHECK(v[1].integer(i));
    CHECK(i == 802816);
    CHECK(v[2].integer(i));
    CHECK(i == -1);
    CHECK(v[3].integer(i));                // 2^53: still exact
    CHECK_FALSE(v[4].integer(i));          // beyond 2^53: a double can't promise exactness
    CHECK_FALSE(v[5].integer(i));          // fractional
    CHECK(v[6].integer(i));                // 1e3 is the integer 1000
    CHECK(i == 1000);
    CHECK(v[7].integer(i));
    CHECK(i == 4294967295);
    CHECK_FALSE(Node().integer(i));
}

TEST_CASE("JSON: strings and escapes", "[json]") {
    Doc d;
    REQUIRE(d.parse(R"(["\"\\\/\b\f\n\r\t", "é€😀", "café"])"));
    const auto v = d.root().items();
    CHECK(v[0].str() == "\"\\/\b\f\n\r\t");
    CHECK(v[1].str() == "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    CHECK(v[2].str() == "caf\xC3\xA9");
    CHECK_FALSE(d.parse(R"("\ude00")"));
    CHECK_FALSE(d.parse(R"("\x")"));
    CHECK_FALSE(d.parse(R"("abc)"));
}

TEST_CASE("JSON: malformed documents and trailing data are rejected", "[json]") {
    Doc d;
    std::string err;
    CHECK_FALSE(d.parse("", &err));
    CHECK_FALSE(d.parse("{", &err));
    CHECK_FALSE(d.parse("{\"a\" 1}", &err));
    CHECK_FALSE(d.parse("[1 2]", &err));
    CHECK_FALSE(d.parse("{a:1}", &err));
    CHECK_FALSE(d.parse("tru", &err));
    CHECK_FALSE(d.parse("{} x", &err));
    CHECK(err.find("offset 3") != std::string::npos);
    CHECK(d.root().missing());  // a failed parse leaves no tree
    CHECK(d.parse(" {} \n"));    // trailing whitespace is fine
    CHECK(d.root().is_object());
}

TEST_CASE("JSON: nesting limit stops deep documents", "[json]") {
    Doc d;
    const auto nested = [](int n) { return std::string(static_cast<size_t>(n), '[') + std::string(static_cast<size_t>(n), ']'); };
    CHECK(d.parse(nested(dfu32::json::kNestingLimit - 1)));
    CHECK_FALSE(d.parse(nested(dfu32::json::kNestingLimit + 1)));
    CHECK_FALSE(d.parse(nested(4000)));  // e.g. an 8 KB hostile manifest: refused, no deep recursion
}

TEST_CASE("JSON: Doc owns and moves its tree", "[json]") {
    Doc a;
    REQUIRE(a.parse(R"({"k":"v"})"));
    Doc b(std::move(a));
    CHECK(b.root().get("k").str() == "v");
    CHECK(a.root().missing());  // NOLINT(bugprone-use-after-move): moved-from is empty by contract
    Doc c;
    REQUIRE(c.parse("[1]"));
    c = std::move(b);
    CHECK(c.root().get("k").str() == "v");
}

TEST_CASE("JSON: object writer escapes and round-trips", "[json]") {
    ObjectWriter w;
    w.add("s", "a \"quoted\"\nline\x01").add("n", int64_t{802816}).add("neg", int64_t{-3}).add_bool("t", true);
    const std::string out = w.str();
    CHECK(out.find("\"n\":802816") != std::string::npos);  // integers print without exponent or fraction
    Doc d;
    REQUIRE(d.parse(out));
    CHECK(d.root().get("s").str() == "a \"quoted\"\nline\x01");
    int64_t i = 0;
    CHECK(d.root().get("neg").integer(i));
    CHECK(i == -3);
    CHECK(d.root().get("t").boolean());
}
