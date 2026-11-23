#pragma once

#include "parcel/core/guid.hpp"

#include <string_view>

namespace parcel::import {

[[nodiscard]] core::AssetGuid DefaultIesAssetGuid(std::string_view project_relative_path);

} // namespace parcel::import
