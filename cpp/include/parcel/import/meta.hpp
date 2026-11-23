#pragma once

#include "parcel/assets/json.hpp"
#include "parcel/core/guid.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::import {

inline constexpr std::uint32_t kAssetMetaVersion = 1;

struct MetaAsset {
    core::AssetGuid guid;
    std::string type;
    std::string role;
    std::string local_id;

    friend bool operator==(const MetaAsset&, const MetaAsset&) = default;
};

struct AssetMeta {
    std::uint32_t version = kAssetMetaVersion;
    core::AssetGuid source_guid;
    std::string importer;
    std::uint32_t importer_version = 1;
    core::Hash256 source_hash;
    core::Hash256 settings_hash;
    assets::JsonValue settings;
    std::vector<MetaAsset> assets;
    std::vector<core::AssetGuid> dependencies;
};

[[nodiscard]] core::Result<AssetMeta> ParseAssetMeta(std::string_view text);
[[nodiscard]] core::Result<AssetMeta> LoadAssetMetaFile(std::string_view path);
[[nodiscard]] core::Result<void> WriteAssetMetaFile(std::string_view path, const AssetMeta& meta);
[[nodiscard]] std::string SerializeAssetMeta(const AssetMeta& meta, bool pretty = true);

[[nodiscard]] std::string AssetMetaPath(std::string_view source_path);

[[nodiscard]] core::AssetGuid MakeSourceGuid(std::string_view project_relative_path);
[[nodiscard]] core::AssetGuid MakeSubAssetGuid(core::AssetGuid source_guid, std::string_view role,
                                               std::string_view local_id);
[[nodiscard]] assets::JsonValue MakeDefaultImportSettings(std::string_view importer,
                                                          std::string_view target);

[[nodiscard]] core::Hash256 HashSettings(const assets::JsonValue& settings);

[[nodiscard]] const assets::JsonValue* FindSetting(const assets::JsonValue& settings,
                                                   std::string_view key) noexcept;
[[nodiscard]] bool SettingBool(const assets::JsonValue& settings, std::string_view key,
                               bool fallback);
[[nodiscard]] double SettingNumber(const assets::JsonValue& settings, std::string_view key,
                                   double fallback);
[[nodiscard]] std::string SettingString(const assets::JsonValue& settings, std::string_view key,
                                        std::string_view fallback);

} // namespace parcel::import
