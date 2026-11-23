#pragma once

#include "parcel/assets/json.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/result.hpp"

#include <string>
#include <vector>

namespace parcel::import::detail {

enum class TextureQuality {
    Fast,
    Balanced,
    Best,
};

struct TextureEncodingOptions {
    std::string format = "auto";
    std::string compression = "none";
    TextureQuality quality = TextureQuality::Balanced;
};

[[nodiscard]] core::Result<TextureEncodingOptions>
ParseTextureEncodingOptions(const assets::JsonValue& settings);

[[nodiscard]] core::Result<assets::TextureFormat>
ResolveTextureFormat(const TextureEncodingOptions& options, bool hdr, bool has_alpha);

[[nodiscard]] bool TextureHasTransparency(const assets::TextureMip& mip);

[[nodiscard]] core::Result<void> EncodeTextureMips(std::vector<assets::TextureMip>& mips,
                                                   bool source_is_float,
                                                   assets::TextureFormat format,
                                                   TextureQuality quality, bool srgb);

} // namespace parcel::import::detail
