// SPDX-License-Identifier: MIT
//
// Small JSON reader for manifests (SPEC.md §11) and HTTP API request bodies.
// Builds a tree; inputs are a few KB at most. Numbers keep an exact int64
// when they are integers (the only kind DFU32's formats use); fractional or
// exponent numbers parse but read back as non-integers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dfu32::json {

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::Null; }
    bool is_bool() const noexcept { return type_ == Type::Bool; }
    bool is_number() const noexcept { return type_ == Type::Number; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_array() const noexcept { return type_ == Type::Array; }
    bool is_object() const noexcept { return type_ == Type::Object; }
    /// A number with no fraction or exponent that fits in int64.
    bool is_integer() const noexcept { return type_ == Type::Number && integral_; }

    bool as_bool(bool fallback = false) const noexcept { return is_bool() ? bool_ : fallback; }
    int64_t as_int(int64_t fallback = 0) const noexcept { return is_integer() ? int_ : fallback; }
    /// The string value; empty for non-strings.
    const std::string& as_string() const noexcept { return str_; }

    /// Array elements, or object member values (in document order).
    const std::vector<Value>& items() const noexcept { return items_; }
    /// Object member names, parallel to items(); empty for non-objects.
    const std::vector<std::string>& keys() const noexcept { return keys_; }
    size_t size() const noexcept { return items_.size(); }

    /// Object member by name (the last one if repeated), or nullptr.
    const Value* find(std::string_view key) const noexcept;

private:
    friend class Parser;
    Type type_ = Type::Null;
    bool bool_ = false;
    bool integral_ = false;
    int64_t int_ = 0;
    std::string str_;
    std::vector<Value> items_;
    std::vector<std::string> keys_;
};

/// Escapes `s` for use inside a JSON string literal (no surrounding quotes).
std::string escape(std::string_view s);

/// Parses one JSON document (RFC 8259) with nothing but whitespace after it.
/// On failure returns false and, if `error` is given, describes the problem
/// and its byte offset.
bool parse(std::string_view text, Value& out, std::string* error = nullptr, unsigned max_depth = 16);

}  // namespace dfu32::json
