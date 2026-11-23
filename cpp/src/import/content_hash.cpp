#include "parcel/import/content_hash.hpp"

#include "parcel/assets/asset_codec.hpp"
#include "parcel/assets/serializers.hpp"

#include <utility>

namespace parcel::import {
namespace {

const assets::TypeRegistry& DedupRegistry() {
    static const assets::TypeRegistry registry = [] {
        assets::TypeRegistry result;
        PARCEL_VERIFY(assets::RegisterStandardAssetSerializers(result));
        return result;
    }();
    return registry;
}

} // namespace

core::Result<core::Hash256> HashMaterialContent(const assets::MaterialAsset& material) {
    assets::MaterialAsset copy = material;
    copy.name.clear();
    auto bytes = assets::EncodeAssetBytes(copy, DedupRegistry());
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    return core::Sha256(bytes.Value());
}

core::Result<core::Hash256> HashMeshContent(const assets::MeshAsset& mesh,
                                            std::span<const core::AssetGuid> materials) {
    assets::MeshAsset copy = mesh;
    copy.name.clear();
    auto bytes = assets::EncodeAssetBytes(copy, DedupRegistry());
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    std::vector<std::byte> combined = std::move(bytes).Value();
    for (const core::AssetGuid& material : materials) {
        const auto* begin = reinterpret_cast<const std::byte*>(material.bytes.data());
        combined.insert(combined.end(), begin, begin + material.bytes.size());
    }
    return core::Sha256(combined);
}

} // namespace parcel::import
