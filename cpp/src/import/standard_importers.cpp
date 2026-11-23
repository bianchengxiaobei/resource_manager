#include "parcel/import/importers.hpp"

#include <memory>

namespace parcel::import {

std::shared_ptr<const IAssetImporter> MakeTextureImporter();
std::shared_ptr<const IAssetImporter> MakeShaderImporter();
std::shared_ptr<const IAssetImporter> MakeTextImporter();
std::shared_ptr<const IAssetImporter> MakeRawFileImporter();
std::shared_ptr<const IAssetImporter> MakeIesProfileImporter();
std::shared_ptr<const IAssetImporter> MakeFbxImporter();
std::shared_ptr<const IAssetImporter> MakeGltfImporter();

core::Result<ImporterRegistry> CreateStandardImporterRegistry() {
    ImporterRegistry registry;
    PARCEL_TRY(registry.Register(MakeTextureImporter()));
    PARCEL_TRY(registry.Register(MakeShaderImporter()));
    PARCEL_TRY(registry.Register(MakeTextImporter()));
    PARCEL_TRY(registry.Register(MakeRawFileImporter()));
    PARCEL_TRY(registry.Register(MakeIesProfileImporter()));
    if (auto fbx = MakeFbxImporter(); fbx) {
        PARCEL_TRY(registry.Register(std::move(fbx)));
    }
    if (auto gltf = MakeGltfImporter(); gltf) {
        PARCEL_TRY(registry.Register(std::move(gltf)));
    }
    return registry;
}

} // namespace parcel::import
