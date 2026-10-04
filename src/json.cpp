#include "haiku_remote/json.hpp"

#include <array>
#include <cmath>
#include <cstdio>

namespace haiku_remote::json {

const Value* Value::find(std::string_view key) const
{
    if (type_ != Type::object)
        return nullptr;
    for (const auto& member : object_) {
        if (member.first == key)
            return &member.second;
    }
    return nullptr;
}

void Value::set(std::string key, Value value)
{
    type_ = Type::object;
    for (auto& member : object_) {
        if (member.first == key) {
            member.second = std::move(value);
            return;
        }
    }
    object_.emplace_back(std::move(key), std::move(value));
}

void Value::push_back(Value value)
{
    type_ = Type::array;
    array_.push_back(std::move(value));
}

namespace {

void append_escaped(std::string& out, const std::string& text)
{
    out.push_back('"');
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    std::array<char, 8> buffer {};
                    std::snprintf(buffer.data(), buffer.size(), "\\u%04x", c);
                    out += buffer.data();
                } else {
                    // Pass valid UTF-8 bytes through untouched.
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

void append_number(std::string& out, double number)
{
    // Emit integral values without a fractional part so ports and timestamps
    // round-trip as plain integers; otherwise use a round-trip-safe format.
    if (std::isfinite(number)
        && number == std::floor(number)
        && std::abs(number) < 1e15) {
        std::array<char, 32> buffer {};
        std::snprintf(buffer.data(), buffer.size(), "%lld",
                      static_cast<long long>(number));
        out += buffer.data();
        return;
    }
    std::array<char, 32> buffer {};
    std::snprintf(buffer.data(), buffer.size(), "%.17g",
                  std::isfinite(number) ? number : 0.0);
    out += buffer.data();
}

void serialize_into(const Value& value, std::string& out, bool pretty, int depth)
{
    const std::string indent = pretty ? std::string(2 * (depth + 1), ' ') : "";
    const std::string closing_indent = pretty ? std::string(2 * depth, ' ') : "";
    const char* newline = pretty ? "\n" : "";
    const char* space = pretty ? " " : "";

    switch (value.type()) {
        case Value::Type::null:
            out += "null";
            break;
        case Value::Type::boolean:
            out += value.as_bool() ? "true" : "false";
            break;
        case Value::Type::number:
            append_number(out, value.as_number());
            break;
        case Value::Type::string:
            append_escaped(out, value.as_string());
            break;
        case Value::Type::array: {
            const Array& array = value.as_array();
            if (array.empty()) {
                out += "[]";
                break;
            }
            out += "[";
            out += newline;
            for (std::size_t i = 0; i < array.size(); ++i) {
                out += indent;
                serialize_into(array[i], out, pretty, depth + 1);
                if (i + 1 < array.size())
                    out += ",";
                out += newline;
            }
            out += closing_indent;
            out += "]";
            break;
        }
        case Value::Type::object: {
            const Object& object = value.as_object();
            if (object.empty()) {
                out += "{}";
                break;
            }
            out += "{";
            out += newline;
            for (std::size_t i = 0; i < object.size(); ++i) {
                out += indent;
                append_escaped(out, object[i].first);
                out += ":";
                out += space;
                serialize_into(object[i].second, out, pretty, depth + 1);
                if (i + 1 < object.size())
                    out += ",";
                out += newline;
            }
            out += closing_indent;
            out += "}";
            break;
        }
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    ParseResult run()
    {
        skip_whitespace();
        Value value;
        if (!parse_value(value))
            return fail();
        skip_whitespace();
        if (pos_ != text_.size()) {
            set_error("trailing data after JSON document");
            return fail();
        }
        ParseResult result;
        result.ok = true;
        result.value = std::move(value);
        result.position = pos_;
        return result;
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
    std::size_t error_pos_ = 0;

    [[nodiscard]] bool at_end() const { return pos_ >= text_.size(); }
    [[nodiscard]] char peek() const { return text_[pos_]; }

    void skip_whitespace()
    {
        while (!at_end()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    bool set_error(const std::string& message)
    {
        if (error_.empty()) {
            error_ = message;
            error_pos_ = pos_;
        }
        return false;
    }

    bool error(const std::string& message) { return set_error(message); }

    ParseResult fail()
    {
        ParseResult result;
        result.ok = false;
        result.error = error_.empty() ? "invalid JSON" : error_;
        result.position = error_pos_;
        return result;
    }

    bool parse_value(Value& out)
    {
        skip_whitespace();
        if (at_end())
            return error("unexpected end of input");
        switch (peek()) {
            case '{': return parse_object(out);
            case '[': return parse_array(out);
            case '"': {
                std::string s;
                if (!parse_string(s))
                    return false;
                out = Value(std::move(s));
                return true;
            }
            case 't':
            case 'f': return parse_bool(out);
            case 'n': return parse_null(out);
            default: return parse_number(out);
        }
    }

    bool parse_literal(std::string_view word)
    {
        if (text_.substr(pos_, word.size()) != word)
            return error("invalid literal");
        pos_ += word.size();
        return true;
    }

    bool parse_bool(Value& out)
    {
        if (peek() == 't') {
            if (!parse_literal("true"))
                return false;
            out = Value(true);
        } else {
            if (!parse_literal("false"))
                return false;
            out = Value(false);
        }
        return true;
    }

    bool parse_null(Value& out)
    {
        if (!parse_literal("null"))
            return false;
        out = Value(nullptr);
        return true;
    }

    bool parse_number(Value& out)
    {
        const std::size_t start = pos_;
        if (!at_end() && peek() == '-')
            ++pos_;
        while (!at_end()) {
            const char c = peek();
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E'
                || c == '+' || c == '-')
                ++pos_;
            else
                break;
        }
        if (pos_ == start)
            return error("expected a value");
        const std::string token(text_.substr(start, pos_ - start));
        try {
            std::size_t consumed = 0;
            const double number = std::stod(token, &consumed);
            if (consumed != token.size())
                return error("malformed number");
            out = Value(number);
        } catch (...) {
            return error("malformed number");
        }
        return true;
    }

    bool parse_string(std::string& out)
    {
        if (peek() != '"')
            return error("expected a string");
        ++pos_;
        out.clear();
        while (!at_end()) {
            const char c = text_[pos_++];
            if (c == '"')
                return true;
            if (c == '\\') {
                if (at_end())
                    return error("unterminated escape");
                const char escape = text_[pos_++];
                switch (escape) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (!parse_unicode_escape(out))
                            return false;
                        break;
                    }
                    default: return error("invalid escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return error("unterminated string");
    }

    bool parse_hex4(unsigned& out)
    {
        if (pos_ + 4 > text_.size())
            return error("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= static_cast<unsigned>(c - 'A' + 10);
            else
                return error("invalid \\u escape");
        }
        return true;
    }

    static void append_utf8(std::string& out, unsigned codepoint)
    {
        if (codepoint <= 0x7f) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }

    bool parse_unicode_escape(std::string& out)
    {
        unsigned code = 0;
        if (!parse_hex4(code))
            return false;
        if (code >= 0xd800 && code <= 0xdbff) {
            // High surrogate: a low surrogate must follow.
            if (pos_ + 2 > text_.size() || text_[pos_] != '\\'
                || text_[pos_ + 1] != 'u')
                return error("unpaired surrogate");
            pos_ += 2;
            unsigned low = 0;
            if (!parse_hex4(low))
                return false;
            if (low < 0xdc00 || low > 0xdfff)
                return error("invalid low surrogate");
            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        }
        append_utf8(out, code);
        return true;
    }

    bool parse_array(Value& out)
    {
        ++pos_; // consume '['
        Array array;
        skip_whitespace();
        if (!at_end() && peek() == ']') {
            ++pos_;
            out = Value(std::move(array));
            return true;
        }
        while (true) {
            Value element;
            if (!parse_value(element))
                return false;
            array.push_back(std::move(element));
            skip_whitespace();
            if (at_end())
                return error("unterminated array");
            const char c = text_[pos_++];
            if (c == ']')
                break;
            if (c != ',')
                return error("expected ',' or ']'");
        }
        out = Value(std::move(array));
        return true;
    }

    bool parse_object(Value& out)
    {
        ++pos_; // consume '{'
        Object object;
        skip_whitespace();
        if (!at_end() && peek() == '}') {
            ++pos_;
            out = Value(std::move(object));
            return true;
        }
        while (true) {
            skip_whitespace();
            if (at_end() || peek() != '"')
                return error("expected a member name");
            std::string key;
            if (!parse_string(key))
                return false;
            skip_whitespace();
            if (at_end() || text_[pos_++] != ':')
                return error("expected ':'");
            Value member;
            if (!parse_value(member))
                return false;
            object.emplace_back(std::move(key), std::move(member));
            skip_whitespace();
            if (at_end())
                return error("unterminated object");
            const char c = text_[pos_++];
            if (c == '}')
                break;
            if (c != ',')
                return error("expected ',' or '}'");
        }
        out = Value(std::move(object));
        return true;
    }
};

} // namespace

ParseResult parse(std::string_view text)
{
    Parser parser(text);
    return parser.run();
}

std::string serialize(const Value& value, bool pretty)
{
    std::string out;
    serialize_into(value, out, pretty, 0);
    if (pretty)
        out.push_back('\n');
    return out;
}

} // namespace haiku_remote::json
