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

std::string_view Node::key() const noexcept {
    return n_ != nullptr && n_->string != nullptr ? std::string_view(n_->string) : std::string_view();
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

ObjectWriter::ObjectWriter(ObjectWriter&& other) noexcept : obj_(other.obj_), ok_(other.ok_) {
    other.obj_ = nullptr;
    other.ok_ = false;
}

cJSON* ObjectWriter::release() noexcept {
    cJSON* o = ok_ ? obj_ : nullptr;
    if (o != nullptr) obj_ = nullptr;
    ok_ = false;
    return o;
}

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

ObjectWriter& ObjectWriter::add_null(const char* key) {
    if (ok_) ok_ = cJSON_AddNullToObject(obj_, key) != nullptr;
    return *this;
}

namespace {

// Attaches `child` (owned) to `parent`; frees it and returns false on failure.
bool attach(cJSON* parent, const char* key, cJSON* child) {
    if (child == nullptr) return false;
    const bool ok = key != nullptr ? cJSON_AddItemToObject(parent, key, child) != 0
                                   : cJSON_AddItemToArray(parent, child) != 0;
    if (!ok) cJSON_Delete(child);
    return ok;
}

cJSON* parse_value(std::string_view json) {
    const std::string buf(json);
    const char* end = nullptr;
    return cJSON_ParseWithLengthOpts(buf.c_str(), buf.size() + 1, &end, 1);
}

std::string print(const cJSON* v, bool ok) {
    if (!ok || v == nullptr) return {};
    char* text = cJSON_PrintUnformatted(v);
    if (text == nullptr) return {};
    std::string out(text);
    cJSON_free(text);
    return out;
}

}  // namespace

ObjectWriter& ObjectWriter::add(const char* key, ObjectWriter&& child) {
    cJSON* c = child.release();
    if (ok_) {
        ok_ = attach(obj_, key, c);
    } else {
        cJSON_Delete(c);
    }
    return *this;
}

ObjectWriter& ObjectWriter::add(const char* key, ArrayWriter&& child) {
    cJSON* c = child.release();
    if (ok_) {
        ok_ = attach(obj_, key, c);
    } else {
        cJSON_Delete(c);
    }
    return *this;
}

ObjectWriter& ObjectWriter::add_json(const char* key, std::string_view json) {
    if (ok_) ok_ = attach(obj_, key, parse_value(json));
    return *this;
}

ArrayWriter::ArrayWriter() : arr_(cJSON_CreateArray()), ok_(arr_ != nullptr) {}

ArrayWriter::~ArrayWriter() { cJSON_Delete(arr_); }

ArrayWriter::ArrayWriter(ArrayWriter&& other) noexcept : arr_(other.arr_), ok_(other.ok_) {
    other.arr_ = nullptr;
    other.ok_ = false;
}

cJSON* ArrayWriter::release() noexcept {
    cJSON* a = ok_ ? arr_ : nullptr;
    if (a != nullptr) arr_ = nullptr;
    ok_ = false;
    return a;
}

ArrayWriter& ArrayWriter::push(std::string_view value) {
    if (ok_) {
        const std::string v(value);
        ok_ = attach(arr_, nullptr, cJSON_CreateString(v.c_str()));
    }
    return *this;
}

ArrayWriter& ArrayWriter::push(ObjectWriter&& child) {
    cJSON* c = child.release();
    if (ok_) {
        ok_ = attach(arr_, nullptr, c);
    } else {
        cJSON_Delete(c);
    }
    return *this;
}

ArrayWriter& ArrayWriter::push_json(std::string_view json) {
    if (ok_) ok_ = attach(arr_, nullptr, parse_value(json));
    return *this;
}

size_t ArrayWriter::size() const noexcept { return ok_ ? static_cast<size_t>(cJSON_GetArraySize(arr_)) : 0; }

std::string ArrayWriter::str() const { return print(arr_, ok_); }

std::string ObjectWriter::str() const { return print(obj_, ok_); }

}  // namespace dfu32::json
