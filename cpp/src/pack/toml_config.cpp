#include "toml_config.hpp"

#include "parcel/core/error.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace parcel::pack::detail {
namespace {

using assets::JsonType;
using assets::JsonValue;
using core::ErrorCode;
using core::MakeError;

bool IsBareKeyChar(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-';
}

bool IsDigit(char value) { return value >= '0' && value <= '9'; }

void AppendUtf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

class TomlParser final {
  public:
    explicit TomlParser(std::string_view text) : text_(text) {}

    core::Result<JsonValue> Parse() {
        JsonValue root;
        root.type = JsonType::Object;
        JsonValue* current = &root;
        while (true) {
            SkipWhitespaceAndComments();
            if (AtEnd()) {
                break;
            }
            if (Peek() == '[') {
                if (!ParseCollectorsTable(root, current)) {
                    return ErrorValue();
                }
                continue;
            }
            std::string key;
            if (!ParseKey(key)) {
                return ErrorValue();
            }
            SkipHorizontalWhitespace();
            if (!Consume('=')) {
                return FailValue("expected '=' after key");
            }
            SkipHorizontalWhitespace();
            JsonValue value;
            if (!ParseValue(value, 0)) {
                return ErrorValue();
            }
            if (FindMutable(*current, key) != nullptr) {
                return FailValue("duplicate key '" + key + "'");
            }
            current->object_value.emplace_back(std::move(key), std::move(value));
            if (!FinishLine()) {
                return ErrorValue();
            }
        }
        return root;
    }

  private:
    core::Result<JsonValue> ErrorValue() const {
        return MakeError(ErrorCode::InvalidArgument, (error_.empty() ? "invalid TOML" : error_) +
                                                         " at line " + std::to_string(line_) +
                                                         ", column " + std::to_string(column_));
    }

    core::Result<JsonValue> FailValue(std::string message) {
        Fail(std::move(message));
        return ErrorValue();
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

    void SkipHorizontalWhitespace() {
        while (!AtEnd() && (Peek() == ' ' || Peek() == '\t')) {
            Take();
        }
    }

    void SkipComment() {
        while (!AtEnd() && Peek() != '\n') {
            Take();
        }
    }

    void SkipWhitespaceAndComments() {
        while (!AtEnd()) {
            const char value = Peek();
            if (value == ' ' || value == '\t' || value == '\r' || value == '\n') {
                Take();
            } else if (value == '#') {
                SkipComment();
            } else {
                break;
            }
        }
    }

    bool FinishLine() {
        SkipHorizontalWhitespace();
        if (!AtEnd() && Peek() == '#') {
            SkipComment();
        }
        if (!AtEnd() && Peek() != '\n' && Peek() != '\r') {
            return Fail("unexpected characters after value");
        }
        return true;
    }

    static JsonValue* FindMutable(JsonValue& object, std::string_view key) {
        for (auto& member : object.object_value) {
            if (member.first == key) {
                return &member.second;
            }
        }
        return nullptr;
    }

    bool ParseKey(std::string& key) {
        if (AtEnd()) {
            return Fail("expected key");
        }
        if (Peek() == '"') {
            return ParseBasicString(key);
        }
        if (Peek() == '\'') {
            return ParseLiteralString(key);
        }
        key.clear();
        while (!AtEnd() && IsBareKeyChar(Peek())) {
            key.push_back(Take());
        }
        if (key.empty()) {
            return Fail("expected key");
        }
        return true;
    }

    bool ParseCollectorsTable(JsonValue& root, JsonValue*& current) {
        Take();
        if (!Consume('[')) {
            return Fail("only [[collectors]] tables are supported");
        }
        SkipHorizontalWhitespace();
        std::string key;
        if (!ParseKey(key)) {
            return false;
        }
        SkipHorizontalWhitespace();
        if (!Consume(']') || !Consume(']')) {
            return Fail("expected ']]' after collector table name");
        }
        if (key != "collectors") {
            return Fail("unsupported table '" + key + "'");
        }

        JsonValue* collectors = FindMutable(root, "collectors");
        if (collectors == nullptr) {
            JsonValue array;
            array.type = JsonType::Array;
            root.object_value.emplace_back("collectors", std::move(array));
            collectors = FindMutable(root, "collectors");
        }
        if (collectors == nullptr || !collectors->IsArray()) {
            return Fail("'collectors' must be an array");
        }
        JsonValue collector;
        collector.type = JsonType::Object;
        collectors->array_value.push_back(std::move(collector));
        current = &collectors->array_value.back();
        return FinishLine();
    }

    bool ParseValue(JsonValue& value, std::size_t depth) {
        if (depth > 32) {
            return Fail("TOML nesting is too deep");
        }
        if (AtEnd()) {
            return Fail("unexpected end of TOML");
        }
        switch (Peek()) {
        case '"':
            value.type = JsonType::String;
            return ParseBasicString(value.string_value);
        case '\'':
            value.type = JsonType::String;
            return ParseLiteralString(value.string_value);
        case '[':
            return ParseArray(value, depth + 1);
        case 't':
            return ParseBool("true", value, true);
        case 'f':
            return ParseBool("false", value, false);
        default:
            return ParseInteger(value);
        }
    }

    bool ParseBool(std::string_view literal, JsonValue& value, bool bool_value) {
        for (const char expected : literal) {
            if (Take() != expected) {
                return Fail("invalid boolean literal");
            }
        }
        value.type = JsonType::Bool;
        value.bool_value = bool_value;
        return true;
    }

    bool ParseInteger(JsonValue& value) {
        bool negative = false;
        if (Consume('+')) {
            negative = false;
        } else if (Consume('-')) {
            negative = true;
        }
        if (AtEnd() || !IsDigit(Peek())) {
            return Fail("expected integer value");
        }

        std::uint64_t magnitude = 0;
        bool saw_digit = false;
        bool last_underscore = false;
        while (!AtEnd()) {
            const char next = Peek();
            if (IsDigit(next)) {
                const std::uint64_t digit = static_cast<std::uint64_t>(next - '0');
                if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
                    return Fail("integer value is out of range");
                }
                magnitude = magnitude * 10U + digit;
                saw_digit = true;
                last_underscore = false;
                Take();
            } else if (next == '_') {
                if (!saw_digit || last_underscore) {
                    return Fail("invalid integer underscore");
                }
                last_underscore = true;
                Take();
            } else {
                break;
            }
        }
        if (!saw_digit || last_underscore) {
            return Fail("invalid integer value");
        }

        const std::uint64_t max_magnitude =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (negative) {
            if (magnitude > max_magnitude + 1U) {
                return Fail("integer value is out of range");
            }
        } else if (magnitude > max_magnitude) {
            return Fail("integer value is out of range");
        }
        if (negative && magnitude == max_magnitude + 1U) {
            value.number_value = static_cast<double>(std::numeric_limits<std::int64_t>::min());
        } else {
            const auto signed_value = negative ? -static_cast<std::int64_t>(magnitude)
                                               : static_cast<std::int64_t>(magnitude);
            value.number_value = static_cast<double>(signed_value);
        }
        value.type = JsonType::Number;
        return true;
    }

    bool ParseArray(JsonValue& value, std::size_t depth) {
        Take();
        value.type = JsonType::Array;
        SkipWhitespaceAndComments();
        if (Consume(']')) {
            return true;
        }
        while (true) {
            JsonValue element;
            if (!ParseValue(element, depth)) {
                return false;
            }
            value.array_value.push_back(std::move(element));
            SkipWhitespaceAndComments();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return Fail("expected ',' or ']' in array");
            }
            SkipWhitespaceAndComments();
            if (Consume(']')) {
                return true;
            }
        }
    }

    bool ParseBasicString(std::string& output) {
        if (!Consume('"')) {
            return Fail("expected string");
        }
        output.clear();
        while (!AtEnd()) {
            const char value = Take();
            if (value == '"') {
                return true;
            }
            if (value == '\n' || value == '\r') {
                return Fail("newline in basic string");
            }
            if (value != '\\') {
                if (static_cast<unsigned char>(value) < 0x20U && value != '\t') {
                    return Fail("control character in string");
                }
                output.push_back(value);
                continue;
            }
            if (AtEnd()) {
                return Fail("unterminated string escape");
            }
            switch (Take()) {
            case '"':
                output.push_back('"');
                break;
            case '\\':
                output.push_back('\\');
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u':
                if (!ParseUnicodeEscape(4, output)) {
                    return false;
                }
                break;
            case 'U':
                if (!ParseUnicodeEscape(8, output)) {
                    return false;
                }
                break;
            default:
                return Fail("invalid string escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseLiteralString(std::string& output) {
        if (!Consume('\'')) {
            return Fail("expected literal string");
        }
        output.clear();
        while (!AtEnd()) {
            const char value = Take();
            if (value == '\'') {
                return true;
            }
            if (value == '\n' || value == '\r') {
                return Fail("newline in literal string");
            }
            output.push_back(value);
        }
        return Fail("unterminated literal string");
    }

    bool ParseUnicodeEscape(std::size_t digits, std::string& output) {
        std::uint32_t codepoint = 0;
        for (std::size_t index = 0; index < digits; ++index) {
            if (AtEnd()) {
                return Fail("short unicode escape");
            }
            const char value = Take();
            std::uint32_t nibble = 0;
            if (value >= '0' && value <= '9') {
                nibble = static_cast<std::uint32_t>(value - '0');
            } else if (value >= 'a' && value <= 'f') {
                nibble = static_cast<std::uint32_t>(value - 'a' + 10);
            } else if (value >= 'A' && value <= 'F') {
                nibble = static_cast<std::uint32_t>(value - 'A' + 10);
            } else {
                return Fail("invalid unicode escape");
            }
            codepoint = (codepoint << 4U) | nibble;
        }
        if (codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return Fail("invalid unicode codepoint");
        }
        AppendUtf8(output, codepoint);
        return true;
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
    std::string error_;
};

} // namespace

core::Result<JsonValue> ParseTomlConfig(std::string_view text) {
    TomlParser parser(text);
    return parser.Parse();
}

} // namespace parcel::pack::detail
