#include "parcel/assets/json.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace parcel::assets {
namespace {

class Parser final {
  public:
    explicit Parser(std::string_view text) : text_(text) {}

    Result<JsonValue> Parse() {
        SkipWhitespace();
        JsonValue value;
        if (!ParseValue(value, 0)) {
            return ErrorValue();
        }
        SkipWhitespace();
        if (!AtEnd()) {
            Fail("unexpected trailing characters");
            return ErrorValue();
        }
        return value;
    }

  private:
    Result<JsonValue> ErrorValue() {
        return core::MakeError(core::ErrorCode::InvalidArgument,
                               error_ + " at line " + std::to_string(line_) + ", column " +
                                   std::to_string(column_));
    }

    bool Fail(std::string message) {
        if (error_.empty()) {
            error_ = std::move(message);
        }
        return false;
    }

    bool AtEnd() const noexcept { return position_ >= text_.size(); }

    char Peek() const noexcept { return AtEnd() ? '\0' : text_[position_]; }

    char Take() {
        if (AtEnd()) {
            return '\0';
        }
        const char value = text_[position_++];
        if (value == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return value;
    }

    bool Consume(char expected) {
        if (Peek() != expected) {
            return false;
        }
        Take();
        return true;
    }

    void SkipWhitespace() {
        while (!AtEnd()) {
            const char value = Peek();
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            Take();
        }
    }

    bool ParseValue(JsonValue& value, std::size_t depth) {
        if (depth > 128) {
            return Fail("JSON nesting is too deep");
        }
        if (AtEnd()) {
            return Fail("unexpected end of JSON");
        }
        switch (Peek()) {
        case '{':
            return ParseObject(value, depth + 1);
        case '[':
            return ParseArray(value, depth + 1);
        case '"':
            value.type = JsonType::String;
            return ParseString(value.string_value);
        case 't':
            return ParseLiteral("true", value, JsonType::Bool, true);
        case 'f':
            return ParseLiteral("false", value, JsonType::Bool, false);
        case 'n':
            return ParseLiteral("null", value, JsonType::Null, false);
        default:
            return ParseNumber(value);
        }
    }

    bool ParseLiteral(std::string_view literal, JsonValue& value, JsonType type, bool bool_value) {
        for (char expected : literal) {
            if (Take() != expected) {
                return Fail("invalid JSON literal");
            }
        }
        value.type = type;
        value.bool_value = bool_value;
        return true;
    }

    bool ParseObject(JsonValue& value, std::size_t depth) {
        Take();
        value.type = JsonType::Object;
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }
        while (true) {
            SkipWhitespace();
            if (Peek() != '"') {
                return Fail("JSON object key must be a string");
            }
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return Fail("expected ':' after JSON object key");
            }
            SkipWhitespace();
            JsonValue member;
            if (!ParseValue(member, depth)) {
                return false;
            }
            for (const auto& existing : value.object_value) {
                if (existing.first == key) {
                    return Fail("duplicate JSON object key");
                }
            }
            value.object_value.emplace_back(std::move(key), std::move(member));
            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                return Fail("expected ',' or '}' in JSON object");
            }
        }
    }

    bool ParseArray(JsonValue& value, std::size_t depth) {
        Take();
        value.type = JsonType::Array;
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }
        while (true) {
            SkipWhitespace();
            JsonValue element;
            if (!ParseValue(element, depth)) {
                return false;
            }
            value.array_value.push_back(std::move(element));
            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return Fail("expected ',' or ']' in JSON array");
            }
        }
    }

    bool ParseString(std::string& out) {
        if (!Consume('"')) {
            return Fail("expected JSON string");
        }
        out.clear();
        while (!AtEnd()) {
            const char value = Take();
            if (value == '"') {
                return true;
            }
            if (static_cast<unsigned char>(value) < 0x20U) {
                return Fail("control character in JSON string");
            }
            if (value != '\\') {
                out.push_back(value);
                continue;
            }
            if (AtEnd()) {
                return Fail("unterminated JSON escape");
            }
            switch (Take()) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                std::uint32_t codepoint = 0;
                if (!ParseHex4(codepoint)) {
                    return false;
                }
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    if (!Consume('\\') || !Consume('u')) {
                        return Fail("missing low surrogate in JSON escape");
                    }
                    std::uint32_t low = 0;
                    if (!ParseHex4(low) || low < 0xdc00U || low > 0xdfffU) {
                        return Fail("invalid low surrogate in JSON escape");
                    }
                    codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) + (low - 0xdc00U);
                } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                    return Fail("unexpected low surrogate in JSON escape");
                }
                AppendUtf8(out, codepoint);
                break;
            }
            default:
                return Fail("invalid JSON escape");
            }
        }
        return Fail("unterminated JSON string");
    }

    bool ParseHex4(std::uint32_t& value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            if (AtEnd()) {
                return Fail("short JSON unicode escape");
            }
            const char digit = Take();
            std::uint32_t nibble = 0;
            if (digit >= '0' && digit <= '9') {
                nibble = static_cast<std::uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                nibble = static_cast<std::uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                nibble = static_cast<std::uint32_t>(digit - 'A' + 10);
            } else {
                return Fail("invalid JSON unicode escape");
            }
            value = (value << 4U) | nibble;
        }
        return true;
    }

    static void AppendUtf8(std::string& out, std::uint32_t codepoint) {
        if (codepoint <= 0x7fU) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffU) {
            out.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            out.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            out.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
    }

    bool ParseNumber(JsonValue& value) {
        const std::size_t start = position_;
        if (Consume('-') && AtEnd()) {
            return Fail("invalid JSON number");
        }
        if (Consume('0')) {
            if (Peek() >= '0' && Peek() <= '9') {
                return Fail("leading zero in JSON number");
            }
        } else {
            if (Peek() < '1' || Peek() > '9') {
                return Fail("invalid JSON value");
            }
            while (Peek() >= '0' && Peek() <= '9') {
                Take();
            }
        }
        if (Consume('.')) {
            if (Peek() < '0' || Peek() > '9') {
                return Fail("missing fraction digits in JSON number");
            }
            while (Peek() >= '0' && Peek() <= '9') {
                Take();
            }
        }
        if (Peek() == 'e' || Peek() == 'E') {
            Take();
            if (Peek() == '+' || Peek() == '-') {
                Take();
            }
            if (Peek() < '0' || Peek() > '9') {
                return Fail("missing exponent digits in JSON number");
            }
            while (Peek() >= '0' && Peek() <= '9') {
                Take();
            }
        }
        double number = 0.0;
        const char* begin = text_.data() + start;
        const char* end = text_.data() + position_;
        const auto result = std::from_chars(begin, end, number);
        if (result.ec != std::errc() || result.ptr != end) {
            return Fail("invalid JSON number");
        }
        value.type = JsonType::Number;
        value.number_value = number;
        return true;
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
    std::string error_;
};

} // namespace

const JsonValue* JsonValue::Find(std::string_view key) const noexcept {
    if (type != JsonType::Object) {
        return nullptr;
    }
    for (const auto& member : object_value) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

Result<JsonValue> ParseJson(std::string_view text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    Parser parser(text);
    return parser.Parse();
}

namespace {

void AppendEscaped(std::string& out, std::string_view value) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20U) {
                out += "\\u00";
                out.push_back(kHex[(byte >> 4U) & 0x0FU]);
                out.push_back(kHex[byte & 0x0FU]);
            } else {
                out.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    out.push_back('"');
}

void AppendIndent(std::string& out, std::size_t depth) { out.append(depth * 2U, ' '); }

void AppendNumber(std::string& out, double value) {
    char buffer[64]{};
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general,
                      std::numeric_limits<double>::max_digits10);
    if (result.ec == std::errc()) {
        out.append(buffer, result.ptr);
    } else {
        out.push_back('0');
    }
}

void AppendValue(std::string& out, const JsonValue& value, bool pretty, std::size_t depth) {
    switch (value.type) {
    case JsonType::Null:
        out += "null";
        break;
    case JsonType::Bool:
        out += value.bool_value ? "true" : "false";
        break;
    case JsonType::Number:
        AppendNumber(out, value.number_value);
        break;
    case JsonType::String:
        AppendEscaped(out, value.string_value);
        break;
    case JsonType::Array:
        out.push_back('[');
        if (!value.array_value.empty()) {
            if (pretty) {
                out.push_back('\n');
            }
            for (std::size_t i = 0; i < value.array_value.size(); ++i) {
                if (i != 0) {
                    out.push_back(',');
                    if (pretty) {
                        out.push_back('\n');
                    }
                }
                if (pretty) {
                    AppendIndent(out, depth + 1);
                }
                AppendValue(out, value.array_value[i], pretty, depth + 1);
            }
            if (pretty) {
                out.push_back('\n');
                AppendIndent(out, depth);
            }
        }
        out.push_back(']');
        break;
    case JsonType::Object:
        out.push_back('{');
        if (!value.object_value.empty()) {
            if (pretty) {
                out.push_back('\n');
            }
            for (std::size_t i = 0; i < value.object_value.size(); ++i) {
                if (i != 0) {
                    out.push_back(',');
                    if (pretty) {
                        out.push_back('\n');
                    }
                }
                if (pretty) {
                    AppendIndent(out, depth + 1);
                }
                AppendEscaped(out, value.object_value[i].first);
                out.push_back(':');
                if (pretty) {
                    out.push_back(' ');
                }
                AppendValue(out, value.object_value[i].second, pretty, depth + 1);
            }
            if (pretty) {
                out.push_back('\n');
                AppendIndent(out, depth);
            }
        }
        out.push_back('}');
        break;
    }
}

} // namespace

std::string SerializeJson(const JsonValue& value, bool pretty) {
    std::string result;
    result.reserve(256);
    AppendValue(result, value, pretty, 0);
    return result;
}

} // namespace parcel::assets
