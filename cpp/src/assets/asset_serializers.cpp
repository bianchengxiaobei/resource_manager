#include "parcel/assets/serializers.hpp"

#include <array>
#include <memory>

namespace parcel::assets {

Result<void> RegisterStandardAssetSerializers(TypeRegistry& registry) {
    const std::array<std::shared_ptr<const ITypeSerializer>, 14> serializers = {
        detail::MakeTextureAssetSerializer(),
        detail::MakeMeshAssetSerializer(),
        detail::MakeMaterialAssetSerializer(),
        detail::MakeShaderLibraryAssetSerializer(),
        detail::MakeRawFileAssetSerializer(),
        detail::MakeTextAssetSerializer(),
        detail::MakePipelineSettingsAssetSerializer(),
        detail::MakeFrameSettingsAssetSerializer(),
        detail::MakePostProcessProfileAssetSerializer(),
        detail::MakeShadowSettingsAssetSerializer(),
        detail::MakeVolumetricFogProfileAssetSerializer(),
        detail::MakeSceneAssetSerializer(),
        detail::MakeIesProfileAssetSerializer(),
        detail::MakeAnimationClipAssetSerializer(),
    };
    for (const auto& serializer : serializers) {
        PARCEL_TRY(registry.Register(serializer));
    }
    return {};
}

} // namespace parcel::assets
