#include "parcel/import/texture_dedup.hpp"

#include "parcel/core/hash.hpp"
#include "parcel/import/meta.hpp"

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

namespace parcel::import {

std::string TextureDedupKey(std::string_view content_hash, std::string_view settings_hash) {
    std::string key;
    key.reserve(content_hash.size() + settings_hash.size() + 1);
    key.append(content_hash);
    key.push_back(':');
    key.append(settings_hash);
    return key;
}

core::AssetGuid TextureAssetGuidForContent(std::string_view content_hash,
                                           std::string_view settings_hash) {
    const core::Hash256 hash =
        core::Sha256("parcel.texture.v1:" + TextureDedupKey(content_hash, settings_hash));
    return core::AssetGuid::FromBytes(
        std::span<const std::byte, 16>(reinterpret_cast<const std::byte*>(hash.bytes.data()), 16));
}

const TextureCanonicalAsset* FindCanonicalTexture(const TextureCanonicalIndex* index,
                                                  std::string_view content_hash,
                                                  std::string_view settings_hash) {
    if (index == nullptr) {
        return nullptr;
    }
    const auto found = index->by_content.find(TextureDedupKey(content_hash, settings_hash));
    return found == index->by_content.end() ? nullptr : &found->second;
}

const TextureCanonicalAsset* FindCanonicalTextureByPath(const TextureCanonicalIndex* index,
                                                        std::string_view project_relative_path) {
    if (index == nullptr) {
        return nullptr;
    }
    const auto found = index->by_path.find(std::string(project_relative_path));
    return found == index->by_path.end() ? nullptr : &found->second;
}

core::Hash256 HashTextureCanonicalIndex(const TextureCanonicalIndex& index) {
    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(index.by_path.size());
    for (const auto& [path, asset] : index.by_path) {
        entries.emplace_back(path, asset.asset_guid.ToHex());
    }
    std::sort(entries.begin(), entries.end());
    std::string data = "parcel.texture.index.v1";
    for (const auto& [path, guid] : entries) {
        data.push_back(0);
        data.append(path);
        data.push_back(0);
        data.append(guid);
    }
    return core::Sha256(data);
}

core::AssetGuid DefaultTextureAssetGuid(std::string_view project_relative_path) {
    return MakeSubAssetGuid(MakeSourceGuid(project_relative_path), "texture", "texture");
}

} // namespace parcel::import
