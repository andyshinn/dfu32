// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "dfu32_core/json.hpp"

using dfu32::json::parse;
using dfu32::json::Value;

TEST_CASE("JSON: objects, arrays and scalars", "[json]") {
    Value v;
    std::string err;
    REQUIRE(parse(R"( {"a": 1, "b": [true, false, null], "c": "x\ty", "d": {"e": -42}} )", v, &err));
    REQUIRE(v.is_object());
    CHECK(v.size() == 4);
    CHECK(v.find("a")->as_int() == 1);
    const Value* b = v.find("b");
    REQUIRE(b->is_array());
    REQUIRE(b->size() == 3);
    CHECK(b->items()[0].as_bool() == true);
    CHECK(b->items()[1].is_bool());
    CHECK(b->items()[2].is_null());
    CHECK(v.find("c")->as_string() == "x\ty");
    CHECK(v.find("d")->find("e")->as_int() == -42);
    CHECK(v.find("missing") == nullptr);
    CHECK(v.keys()[3] == "d");
}

TEST_CASE("JSON: numbers", "[json]") {
    Value v;
    REQUIRE(parse("9223372036854775807", v));
    CHECK(v.as_int() == INT64_MAX);
    REQUIRE(parse("-9223372036854775808", v));
    CHECK(v.as_int() == INT64_MIN);
    REQUIRE(parse("9223372036854775808", v));  // too big: a number, not an integer
    CHECK(v.is_number());
    CHECK_FALSE(v.is_integer());
    REQUIRE(parse("1.5", v));
    CHECK_FALSE(v.is_integer());
    CHECK(v.as_int(7) == 7);
    REQUIRE(parse("1e3", v));
    CHECK_FALSE(v.is_integer());
    REQUIRE(parse("0", v));
    CHECK(v.as_int(9) == 0);
    CHECK_FALSE(parse("01", v));
    CHECK_FALSE(parse("-", v));
    CHECK_FALSE(parse("1.", v));
    CHECK_FALSE(parse(".5", v));
    CHECK_FALSE(parse("+1", v));
    CHECK_FALSE(parse("1e", v));
}

TEST_CASE("JSON: string escapes and UTF-8", "[json]") {
    Value v;
    REQUIRE(parse(R"("\"\\\/\b\f\n\r\t")", v));
    CHECK(v.as_string() == "\"\\/\b\f\n\r\t");
    REQUIRE(parse(R"("\u00e9\u20ac\ud83d\ude00")", v));
    CHECK(v.as_string() == "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    REQUIRE(parse("\"caf\xC3\xA9\"", v));  // raw UTF-8 passes through
    CHECK(v.as_string() == "caf\xC3\xA9");
    CHECK_FALSE(parse(R"("\ud83d")", v));
    CHECK_FALSE(parse(R"("\ude00")", v));
    CHECK_FALSE(parse(R"("\x")", v));
    CHECK_FALSE(parse(R"("\u12")", v));
    CHECK_FALSE(parse("\"a\nb\"", v));
    CHECK_FALSE(parse("\"abc", v));
}

TEST_CASE("JSON: malformed documents are rejected with a position", "[json]") {
    Value v;
    std::string err;
    CHECK_FALSE(parse("", v, &err));
    CHECK_FALSE(parse("{", v, &err));
    CHECK_FALSE(parse("{\"a\" 1}", v, &err));
    CHECK(err.find("':'") != std::string::npos);
    CHECK_FALSE(parse("{\"a\":1,}", v, &err));
    CHECK_FALSE(parse("[1,]", v, &err));
    CHECK_FALSE(parse("[1 2]", v, &err));
    CHECK_FALSE(parse("{a:1}", v, &err));
    CHECK_FALSE(parse("tru", v, &err));
    CHECK_FALSE(parse("{} x", v, &err));
    CHECK(err.find("offset 3") != std::string::npos);
    CHECK(v.is_null());  // output reset on failure
}

TEST_CASE("JSON: depth limit and duplicate keys", "[json]") {
    Value v;
    std::string deep(20, '[');
    deep += std::string(20, ']');
    CHECK_FALSE(parse(deep, v));
    CHECK(parse(deep, v, nullptr, 32));
    REQUIRE(parse(R"({"k":1,"k":2})", v));
    CHECK(v.find("k")->as_int() == 2);  // last wins
    REQUIRE(parse("[]", v));
    CHECK(v.is_array());
    CHECK(v.size() == 0);
    REQUIRE(parse(" {} ", v));
    CHECK(v.is_object());
}
