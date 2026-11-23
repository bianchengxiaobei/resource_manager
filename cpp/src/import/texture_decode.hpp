#pragma once

#include "parcel/assets/json.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/result.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace parcel::import {

[[nodiscard]] core::Result<std::shared_ptr<assets::TextureAsset>>
DecodeTextureAsset(std::span<const std::byte> bytes, const assets::JsonValue& settings,
                   std::string_view source_label = {});

} // namespace parcel::import
