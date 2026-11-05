// SPDX-License-Identifier: MIT
#include "dfu32_core/json.hpp"

#include <cstdio>
#include <limits>

namespace dfu32::json {

const Value* Value::find(std::string_view key) const noexcept {
    for (size_t i = keys_.size(); i-- > 0;) {
        if (keys_[i] == key) return &items_[i];
    }
    return nullptr;
}

class Parser {
public:
    Parser(std::string_view text, unsigned max_depth) : s_(text), max_depth_(max_depth) {}

    bool run(Value& out) {
        skip_ws();
        if (!value(out, 0)) return false;
        skip_ws();
        if (pos_ != s_.size()) return fail("unexpected data after the document");
        return true;
    }

    std::string error() const {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s at offset %u", err_ ? err_ : "invalid JSON", static_cast<unsigned>(pos_));
        return buf;
    }

private:
    bool fail(const char* why) {
        if (err_ == nullptr) err_ = why;
        return false;
    }
    bool eof() const { return pos_ >= s_.size(); }
    char peek() const { return eof() ? '\0' : s_[pos_]; }
    void skip_ws() {
        while (!eof() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }
    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return fail("invalid literal");
        pos_ += word.size();
        return true;
    }

    bool value(Value& v, unsigned depth) {
        if (eof()) return fail("unexpected end of input");
        switch (peek()) {
            case '{': return object(v, depth);
            case '[': return array(v, depth);
            case '"': v.type_ = Type::String; return string(v.str_);
            case 't': v.type_ = Type::Bool; v.bool_ = true; return literal("true");
            case 'f': v.type_ = Type::Bool; v.bool_ = false; return literal("false");
            case 'n': v.type_ = Type::Null; return literal("null");
            default: return number(v);
        }
    }

    bool object(Value& v, unsigned depth) {
        if (depth >= max_depth_) return fail("nesting too deep");
        v.type_ = Type::Object;
        ++pos_;  // '{'
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_ws();
            if (peek() != '"') return fail("expected a member name");
            std::string key;
            if (!string(key)) return false;
            skip_ws();
            if (peek() != ':') return fail("expected ':'");
            ++pos_;
            skip_ws();
            Value member;
            if (!value(member, depth + 1)) return false;
            v.keys_.push_back(std::move(key));
            v.items_.push_back(std::move(member));
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Value& v, unsigned depth) {
        if (depth >= max_depth_) return fail("nesting too deep");
        v.type_ = Type::Array;
        ++pos_;  // '['
        skip_ws();
        if (peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_ws();
            Value item;
            if (!value(item, depth + 1)) return false;
            v.items_.push_back(std::move(item));
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    static int hexval(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool hex4(uint32_t& out) {
        if (s_.size() - pos_ < 4) return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hexval(s_[pos_ + static_cast<size_t>(i)]);
            if (h < 0) return fail("bad \\u escape");
            out = (out << 4) | static_cast<uint32_t>(h);
        }
        pos_ += 4;
        return true;
    }

    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool string(std::string& out) {
        ++pos_;  // opening quote
        for (;;) {
            if (eof()) return fail("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (eof()) return fail("unterminated string");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo = 0;
                        if (s_.substr(pos_, 2) != "\\u") return fail("unpaired surrogate");
                        pos_ += 2;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("unpaired surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired surrogate");
                    }
                    utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
    }

    static bool digit(char c) { return c >= '0' && c <= '9'; }

    bool number(Value& v) {
        v.type_ = Type::Number;
        bool neg = false;
        if (peek() == '-') {
            neg = true;
            ++pos_;
        }
        if (!digit(peek())) return fail("invalid value");
        // Accumulate the magnitude as unsigned; -2^63 must still fit.
        uint64_t mag = 0;
        bool overflow = false;
        if (peek() == '0') {
            ++pos_;
            if (digit(peek())) return fail("leading zero");
        } else {
            while (digit(peek())) {
                const auto d = static_cast<uint64_t>(s_[pos_++] - '0');
                if (mag > (std::numeric_limits<uint64_t>::max() - d) / 10) overflow = true;
                mag = mag * 10 + d;
            }
        }
        bool integral = true;
        if (peek() == '.') {
            integral = false;
            ++pos_;
            if (!digit(peek())) return fail("digit expected after '.'");
            while (digit(peek())) ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            integral = false;
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!digit(peek())) return fail("digit expected in exponent");
            while (digit(peek())) ++pos_;
        }
        constexpr uint64_t kMaxPos = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        if (integral && !overflow && (neg ? mag <= kMaxPos + 1 : mag <= kMaxPos)) {
            v.integral_ = true;
            v.int_ = neg ? (mag == kMaxPos + 1 ? std::numeric_limits<int64_t>::min() : -static_cast<int64_t>(mag))
                         : static_cast<int64_t>(mag);
        }
        return true;
    }

    std::string_view s_;
    unsigned max_depth_;
    size_t pos_ = 0;
    const char* err_ = nullptr;
};

bool parse(std::string_view text, Value& out, std::string* error, unsigned max_depth) {
    out = Value{};
    Parser p(text, max_depth);
    if (p.run(out)) return true;
    if (error != nullptr) *error = p.error();
    out = Value{};
    return false;
}

}  // namespace dfu32::json
