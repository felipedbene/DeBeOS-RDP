#pragma once

// Minimal, dependency-free JSON for the connection library's flat schema.
//
// This is the single swap-in seam: ProfileStore talks to JSON only through this
// header, so replacing it with a third-party parser later is a localised change.
// It is deliberately NOT a general-purpose library -- just enough to parse and
// re-emit the object-of-scalars / array-of-objects shape that connections.json
// uses. Object member order is preserved so serialization is deterministic (an
// atomic rewrite that reorders keys would churn the file and defeat diffing).

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace haiku_remote::json {

class Value;

using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
using Object = std::vector<Member>;

class Value {
public:
    enum class Type { null, boolean, number, string, array, object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool value) : type_(Type::boolean), bool_(value) {}
    Value(int value) : type_(Type::number), number_(value) {}
    Value(std::int64_t value)
        : type_(Type::number), number_(static_cast<double>(value)) {}
    Value(double value) : type_(Type::number), number_(value) {}
    Value(const char* value) : type_(Type::string), string_(value) {}
    Value(std::string value) : type_(Type::string), string_(std::move(value)) {}
    Value(Array value) : type_(Type::array), array_(std::move(value)) {}
    Value(Object value) : type_(Type::object), object_(std::move(value)) {}

    [[nodiscard]] Type type() const { return type_; }
    [[nodiscard]] bool is_null() const { return type_ == Type::null; }
    [[nodiscard]] bool is_number() const { return type_ == Type::number; }
    [[nodiscard]] bool is_string() const { return type_ == Type::string; }
    [[nodiscard]] bool is_array() const { return type_ == Type::array; }
    [[nodiscard]] bool is_object() const { return type_ == Type::object; }

    [[nodiscard]] bool as_bool(bool fallback = false) const
    {
        return type_ == Type::boolean ? bool_ : fallback;
    }
    [[nodiscard]] double as_number(double fallback = 0) const
    {
        return type_ == Type::number ? number_ : fallback;
    }
    [[nodiscard]] std::int64_t as_int(std::int64_t fallback = 0) const
    {
        return type_ == Type::number ? static_cast<std::int64_t>(number_)
                                     : fallback;
    }
    [[nodiscard]] std::string as_string(std::string fallback = {}) const
    {
        return type_ == Type::string ? string_ : fallback;
    }
    [[nodiscard]] const Array& as_array() const { return array_; }
    [[nodiscard]] const Object& as_object() const { return object_; }

    // Object member lookup; null for a non-object or an absent key.
    [[nodiscard]] const Value* find(std::string_view key) const;

    // Builders.
    void set(std::string key, Value value); // object: append or replace
    void push_back(Value value);            // array: append

private:
    Type type_ = Type::null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    Array array_;
    Object object_;
};

struct ParseResult {
    bool ok = false;
    Value value;
    std::string error;
    std::size_t position = 0; // byte offset where parsing stopped on error
};

// Parse a complete JSON document. Trailing non-whitespace is an error.
[[nodiscard]] ParseResult parse(std::string_view text);

// Serialize. `pretty` emits two-space indentation and newlines; otherwise a
// compact single line. Member order is preserved.
[[nodiscard]] std::string serialize(const Value& value, bool pretty = true);

} // namespace haiku_remote::json
