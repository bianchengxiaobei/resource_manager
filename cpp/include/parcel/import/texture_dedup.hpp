#pragma once

#include "parcel/core/guid.hpp"
#include "parcel/core/hash.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace parcel::import {

struct TextureCanonicalAsset {
    std::string relative_path;
    core::AssetGuid asset_guid;
};

struct TextureCanonicalIndex {
    std::unordered_map<std::string, TextureCanonicalAsset> by_content;
    std::unordered_map<std::string, TextureCanonicalAsset> by_path;
};

[[nodiscard]] std::string TextureDedupKey(std::string_view content_hash,
                                          std::string_view settings_hash);

[[nodiscard]] const TextureCanonicalAsset* FindCanonicalTexture(const TextureCanonicalIndex* index,
                                                                std::string_view content_hash,
                                                                std::string_view settings_hash);

[[nodiscard]] core::AssetGuid TextureAssetGuidForContent(std::string_view content_hash,
                                                         std::string_view settings_hash);

[[nodiscard]] const TextureCanonicalAsset*
FindCanonicalTextureByPath(const TextureCanonicalIndex* index,
                           std::string_view project_relative_path);

[[nodiscard]] core::Hash256 HashTextureCanonicalIndex(const TextureCanonicalIndex& index);

[[nodiscard]] core::AssetGuid DefaultTextureAssetGuid(std::string_view project_relative_path);

} // namespace parcel::import
