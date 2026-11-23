#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/mesh_asset.hpp"
#include "parcel/assets/raw_file_asset.hpp"
#include "parcel/assets/scene_asset.hpp"
#include "parcel/assets/settings_assets.hpp"
#include "parcel/assets/shader_library_asset.hpp"
#include "parcel/assets/texture_asset.hpp"

namespace parcel::assets {

template <typename T> struct AssetTypeOf {
    static constexpr TypeId value = kInvalidTypeId;
};

#define PARCEL_RUNTIME_ASSET_TYPE(type_name, type_id)                                              \
    template <> struct AssetTypeOf<type_name> {                                                    \
        static constexpr TypeId value = type_id;                                                   \
    }

PARCEL_RUNTIME_ASSET_TYPE(TextureAsset, kTextureAssetType);
PARCEL_RUNTIME_ASSET_TYPE(MeshAsset, kMeshAssetType);
PARCEL_RUNTIME_ASSET_TYPE(MaterialAsset, kMaterialAssetType);
PARCEL_RUNTIME_ASSET_TYPE(ShaderLibraryAsset, kShaderLibraryAssetType);
PARCEL_RUNTIME_ASSET_TYPE(RawFileAsset, kRawFileAssetType);
PARCEL_RUNTIME_ASSET_TYPE(TextAsset, kTextAssetType);
PARCEL_RUNTIME_ASSET_TYPE(PipelineSettingsAsset, kPipelineSettingsAssetType);
PARCEL_RUNTIME_ASSET_TYPE(FrameSettingsAsset, kFrameSettingsAssetType);
PARCEL_RUNTIME_ASSET_TYPE(PostProcessProfileAsset, kPostProcessProfileAssetType);
PARCEL_RUNTIME_ASSET_TYPE(ShadowSettingsAsset, kShadowSettingsAssetType);
PARCEL_RUNTIME_ASSET_TYPE(VolumetricFogProfileAsset, kVolumetricFogProfileAssetType);
PARCEL_RUNTIME_ASSET_TYPE(SceneAsset, kSceneAssetType);

#undef PARCEL_RUNTIME_ASSET_TYPE

} // namespace parcel::assets
