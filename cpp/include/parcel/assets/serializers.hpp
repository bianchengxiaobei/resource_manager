#pragma once

#include "parcel/assets/asset.hpp"

#include <memory>

namespace parcel::assets {

namespace detail {
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakeTextureAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakeMeshAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakeMaterialAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakeShaderLibraryAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakeRawFileAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakeTextAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakePipelineSettingsAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakeFrameSettingsAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakePostProcessProfileAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakeShadowSettingsAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakeVolumetricFogProfileAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakeSceneAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakeIesProfileAssetSerializer();
[[nodiscard]] std::shared_ptr<const ITypeSerializer> MakeAnimationClipAssetSerializer();
} // namespace detail

[[nodiscard]] Result<void> RegisterStandardAssetSerializers(TypeRegistry& registry);

} // namespace parcel::assets
