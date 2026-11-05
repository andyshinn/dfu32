// SPDX-License-Identifier: MIT
//
// Thin C++ wrapper over cJSON (espressif/cjson in the firmware, the same
// upstream release fetched for host tests). It adds ownership (Doc frees the
// tree), case-sensitive member lookup, exact-integer reads (cJSON keeps
// numbers as double), whole-document parsing with an error offset, and a
// small object writer. cJSON itself stays out of this header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct cJSON;

namespace dfu32::json {

/// Maximum nesting accepted (CONFIG_CJSON_NESTING_LIMIT / CJSON_NESTING_LIMIT
/// must be set to this; cJSON's default of 1000 could overflow a task stack).
inline constexpr int kNestingLimit = 16;

/// Non-owning view of a node inside a Doc. A default/absent Node is "missing".
class Node {
public:
    Node() = default;
    explicit Node(const cJSON* n) : n_(n) {}

    bool missing() const noexcept { return n_ == nullptr; }
    bool is_null() const noexcept;
    bool is_bool() const noexcept;
    bool is_number() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    /// Object member by exact (case-sensitive) name; missing if absent.
    Node get(const char* key) const noexcept;
    /// Array elements, or object member values, in document order.
    std::vector<Node> items() const;
    size_t size() const noexcept;

    /// String value; empty for non-strings.
    std::string str() const;
    bool boolean(bool fallback = false) const noexcept;
    /// True if this is a number with an integral value within +-2^53 (the
    /// range a double holds exactly); writes it to `out`.
    bool integer(int64_t& out) const noexcept;

private:
    const cJSON* n_ = nullptr;
};

/// Owns a parsed document.
class Doc {
public:
    Doc() = default;
    ~Doc();
    Doc(Doc&& other) noexcept;
    Doc& operator=(Doc&& other) noexcept;
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;

    /// Parses one JSON document with nothing but whitespace after it. On
    /// failure returns false and, if `error` is given, the byte offset.
    bool parse(std::string_view text, std::string* error = nullptr);
    Node root() const noexcept { return Node(root_); }

private:
    cJSON* root_ = nullptr;
};

/// Builds a flat JSON object.
class ObjectWriter {
public:
    ObjectWriter();
    ~ObjectWriter();
    ObjectWriter(const ObjectWriter&) = delete;
    ObjectWriter& operator=(const ObjectWriter&) = delete;

    ObjectWriter& add(const char* key, std::string_view value);
    /// Integers print exactly up to INT32_MAX (cJSON uses %d there, %g above).
    ObjectWriter& add(const char* key, int64_t value);
    ObjectWriter& add_bool(const char* key, bool value);
    /// Compact JSON; empty if any allocation failed along the way.
    std::string str() const;

private:
    cJSON* obj_;
    bool ok_;
};

}  // namespace dfu32::json
