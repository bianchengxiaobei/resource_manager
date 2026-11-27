#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/serialize/asset_file.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace parcel::assets {

[[nodiscard]] Result<serialize::AssetFileDesc> EncodeAsset(const AssetObject& object,
                                                           const TypeRegistry& registry);

[[nodiscard]] Result<std::shared_ptr<AssetObject>>
DecodeAsset(const serialize::AssetFileReader& reader, const TypeRegistry& registry,
            IObjectResolver* resolver = nullptr);

[[nodiscard]] Result<std::shared_ptr<AssetObject>>
DecodeAssetBytes(std::span<const std::byte> data, const TypeRegistry& registry,
                 IObjectResolver* resolver = nullptr, bool verify_content_hash = true);

struct DecodedAssetFile {
    std::vector<std::shared_ptr<AssetObject>> objects;
    std::vector<std::uint32_t> local_ids;
    std::vector<std::uint32_t> sub_asset_ids;
};

[[nodiscard]] Result<DecodedAssetFile> DecodeAssetFileBytes(std::span<const std::byte> data,
                                                            const TypeRegistry& registry,
                                                            IObjectResolver* resolver = nullptr,
                                                            bool verify_content_hash = true);

[[nodiscard]] Result<std::vector<std::byte>> EncodeAssetBytes(const AssetObject& object,
                                                              const TypeRegistry& registry);

} // namespace parcel::assets
