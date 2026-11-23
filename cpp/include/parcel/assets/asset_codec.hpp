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

[[nodiscard]] Result<std::vector<std::byte>> EncodeAssetBytes(const AssetObject& object,
                                                              const TypeRegistry& registry);

} // namespace parcel::assets
