// SPDX-License-Identifier: MIT
#include "dfu32_core/json.hpp"

#include <cmath>
#include <cstdlib>

#include "cJSON.h"

static_assert(CJSON_NESTING_LIMIT == dfu32::json::kNestingLimit,
              "set CONFIG_CJSON_NESTING_LIMIT (firmware) / CJSON_NESTING_LIMIT (host) to json::kNestingLimit");

namespace dfu32::json {

bool Node::is_null() const noexcept { return n_ != nullptr && cJSON_IsNull(n_); }
bool Node::is_bool() const noexcept { return n_ != nullptr && cJSON_IsBool(n_); }
bool Node::is_number() const noexcept { return n_ != nullptr && cJSON_IsNumber(n_); }
bool Node::is_string() const noexcept { return n_ != nullptr && cJSON_IsString(n_); }
bool Node::is_array() const noexcept { return n_ != nullptr && cJSON_IsArray(n_); }
bool Node::is_object() const noexcept { return n_ != nullptr && cJSON_IsObject(n_); }

Node Node::get(const char* key) const noexcept {
    if (!is_object()) return {};
    return Node(cJSON_GetObjectItemCaseSensitive(n_, key));
}

std::vector<Node> Node::items() const {
    std::vector<Node> out;
    if (!is_array() && !is_object()) return out;
    for (const cJSON* c = n_->child; c != nullptr; c = c->next) out.emplace_back(c);
    return out;
}

size_t Node::size() const noexcept {
    if (!is_array() && !is_object()) return 0;
    return static_cast<size_t>(cJSON_GetArraySize(n_));
}

std::string Node::str() const {
    return is_string() && n_->valuestring != nullptr ? std::string(n_->valuestring) : std::string();
}

bool Node::boolean(bool fallback) const noexcept { return is_bool() ? cJSON_IsTrue(n_) != 0 : fallback; }

bool Node::integer(int64_t& out) const noexcept {
    if (!is_number()) return false;
    const double d = n_->valuedouble;
    constexpr double kExact = 9007199254740992.0;  // 2^53
    if (!std::isfinite(d) || d != std::floor(d) || std::fabs(d) > kExact) return false;
    out = static_cast<int64_t>(d);
    return true;
}

Doc::~Doc() { cJSON_Delete(root_); }

Doc::Doc(Doc&& other) noexcept : root_(other.root_) { other.root_ = nullptr; }

Doc& Doc::operator=(Doc&& other) noexcept {
    if (this != &other) {
        cJSON_Delete(root_);
        root_ = other.root_;
        other.root_ = nullptr;
    }
    return *this;
}

bool Doc::parse(std::string_view text, std::string* error) {
    cJSON_Delete(root_);
    root_ = nullptr;
    // cJSON's "require null terminated" mode wants the NUL inside the given
    // length; that is also what rejects trailing garbage.
    const std::string buf(text);
    const char* end = nullptr;
    root_ = cJSON_ParseWithLengthOpts(buf.c_str(), buf.size() + 1, &end, 1);
    if (root_ != nullptr) return true;
    if (error != nullptr) {
        const size_t at = end != nullptr && end >= buf.c_str() ? static_cast<size_t>(end - buf.c_str()) : 0;
        *error = "invalid JSON at offset " + std::to_string(at);
    }
    return false;
}

ObjectWriter::ObjectWriter() : obj_(cJSON_CreateObject()), ok_(obj_ != nullptr) {}

ObjectWriter::~ObjectWriter() { cJSON_Delete(obj_); }

ObjectWriter& ObjectWriter::add(const char* key, std::string_view value) {
    if (ok_) {
        const std::string v(value);
        ok_ = cJSON_AddStringToObject(obj_, key, v.c_str()) != nullptr;
    }
    return *this;
}

ObjectWriter& ObjectWriter::add(const char* key, int64_t value) {
    if (ok_) ok_ = cJSON_AddNumberToObject(obj_, key, static_cast<double>(value)) != nullptr;
    return *this;
}

ObjectWriter& ObjectWriter::add_bool(const char* key, bool value) {
    if (ok_) ok_ = cJSON_AddBoolToObject(obj_, key, value ? 1 : 0) != nullptr;
    return *this;
}

std::string ObjectWriter::str() const {
    if (!ok_) return {};
    char* text = cJSON_PrintUnformatted(obj_);
    if (text == nullptr) return {};
    std::string out(text);
    cJSON_free(text);
    return out;
}

}  // namespace dfu32::json
