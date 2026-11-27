#pragma once

#include "parcel/assets/json.hpp"
#include "parcel/core/result.hpp"

#include <string_view>

namespace parcel::pack::detail {

[[nodiscard]] core::Result<assets::JsonValue> ParseTomlConfig(std::string_view text);

} // namespace parcel::pack::detail
