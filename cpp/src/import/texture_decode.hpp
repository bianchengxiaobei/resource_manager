#pragma once

#include "parcel/assets/json.hpp"
#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/result.hpp"
#include "texture_transform.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace parcel::import {

struct TextureBakeOptions {
    TextureUvTransform transform;
    assets::TextureWrap wrap_s = assets::TextureWrap::Repeat;
    assets::TextureWrap wrap_t = assets::TextureWrap::Repeat;
    bool bilinear = true;
};

[[nodiscard]] core::Result<std::shared_ptr<assets::TextureAsset>>
DecodeTextureAsset(std::span<const std::byte> bytes, const assets::JsonValue& settings,
                   std::string_view source_label = {}, const TextureBakeOptions* bake = nullptr);

} // namespace parcel::import
