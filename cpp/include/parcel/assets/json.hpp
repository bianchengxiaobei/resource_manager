#pragma once

#include "parcel/core/result.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace parcel::assets {

using core::Result;

enum class JsonType { Null, Bool, Number, String, Array, Object };

class JsonValue {
  public:
    JsonType type = JsonType::Null;
    bool bool_value = false;
    double number_value = 0.0;
    std::string string_value;
    std::vector<JsonValue> array_value;
    std::vector<std::pair<std::string, JsonValue>> object_value;

    [[nodiscard]] bool IsNull() const noexcept { return type == JsonType::Null; }
    [[nodiscard]] bool IsBool() const noexcept { return type == JsonType::Bool; }
    [[nodiscard]] bool IsNumber() const noexcept { return type == JsonType::Number; }
    [[nodiscard]] bool IsString() const noexcept { return type == JsonType::String; }
    [[nodiscard]] bool IsArray() const noexcept { return type == JsonType::Array; }
    [[nodiscard]] bool IsObject() const noexcept { return type == JsonType::Object; }

    [[nodiscard]] const JsonValue* Find(std::string_view key) const noexcept;
};

[[nodiscard]] core::Result<JsonValue> ParseJson(std::string_view text);

[[nodiscard]] std::string SerializeJson(const JsonValue& value, bool pretty = false);

} // namespace parcel::assets
