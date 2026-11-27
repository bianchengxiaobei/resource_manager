#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/assets/json.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/guid.hpp"
#include "parcel/core/result.hpp"
#include "parcel/import/texture_dedup.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::import {

using TextureDecodeFallback = std::shared_ptr<assets::TextureAsset> (*)(const std::byte* data,
                                                                        std::size_t size,
                                                                        void* user);

struct ImportRequest {
    std::string project_root;
    std::string source_path;
    std::string project_relative_path;
    core::AssetGuid source_guid;
    const assets::JsonValue* settings = nullptr;
    const TextureCanonicalIndex* texture_index = nullptr;
    TextureDecodeFallback texture_decode_fallback = nullptr;
    void* texture_decode_user = nullptr;
};

struct CookedAsset {
    core::AssetGuid guid;
    std::string local_id;
    std::string role;
    std::shared_ptr<assets::AssetObject> object;
    std::vector<core::AssetGuid> dependencies;
};

struct ImportResult {
    std::vector<CookedAsset> assets;
    std::vector<core::AssetGuid> dependencies;
    std::vector<std::string> warnings;
};

class IAssetImporter {
  public:
    virtual ~IAssetImporter() = default;

    [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t Version() const noexcept = 0;
    [[nodiscard]] virtual bool Supports(std::string_view extension) const noexcept = 0;
    [[nodiscard]] virtual core::Result<ImportResult> Import(const ImportRequest& request) const = 0;
};

class ImporterRegistry {
  public:
    [[nodiscard]] core::Result<void> Register(std::shared_ptr<const IAssetImporter> importer);
    [[nodiscard]] const IAssetImporter* FindByName(std::string_view name) const noexcept;
    [[nodiscard]] const IAssetImporter* FindForExtension(std::string_view extension) const noexcept;
    [[nodiscard]] bool Empty() const noexcept { return importers_.empty(); }
    [[nodiscard]] std::span<const std::shared_ptr<const IAssetImporter>>
    Importers() const noexcept {
        return importers_;
    }

  private:
    std::vector<std::shared_ptr<const IAssetImporter>> importers_;
};

} // namespace parcel::import
