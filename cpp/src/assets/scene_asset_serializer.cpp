#include "parcel/assets/scene_asset.hpp"
#include "parcel/assets/serializers.hpp"
#include "parcel/scene/scene_data.hpp"
#include "serializer_util.hpp"

#include <memory>

namespace parcel::assets::detail {
namespace {

class SceneAssetSerializer final : public ITypeSerializer {
  public:
    TypeId Type() const noexcept override { return kSceneAssetType; }
    std::string_view TypeName() const noexcept override { return "SceneAsset"; }
    std::uint32_t Version() const noexcept override { return 1; }

    Result<std::shared_ptr<AssetObject>> Create() const override {
        return std::static_pointer_cast<AssetObject>(std::make_shared<SceneAsset>());
    }

    Result<void> Serialize(const AssetObject& object, BinaryWriter& writer,
                           IAssetWriteContext& context) const override {
        const auto& asset = static_cast<const SceneAsset&>(object);
        PARCEL_TRY_DECL(std::vector<std::byte>, encoded,
                        scene::EncodeScene(asset.scene, scene::StandardComponentRegistry()));
        PARCEL_TRY(writer.WriteString(asset.name));
        PARCEL_TRY_DECL(std::uint64_t, blob_id,
                        WriteBlob(context, std::span<const std::byte>(encoded)));
        PARCEL_TRY(writer.WriteU64(blob_id));
        return {};
    }

    Result<std::shared_ptr<AssetObject>> Deserialize(BinaryReader& reader,
                                                     IAssetReadContext& context) const override {
        auto asset = std::make_shared<SceneAsset>();
        PARCEL_TRY_DECL(std::string_view, name, reader.ReadString());
        asset->name.assign(name);
        PARCEL_TRY_DECL(std::uint64_t, blob_id, reader.ReadU64());
        PARCEL_TRY_DECL(std::span<const std::byte>, encoded, ReadBlob(context, blob_id));
        PARCEL_TRY_ASSIGN(asset->scene,
                          scene::DecodeScene(encoded, scene::StandardComponentRegistry()));
        PARCEL_TRY(Finish(reader, "scene metadata"));
        return std::static_pointer_cast<AssetObject>(asset);
    }

    Result<void> CollectReferences(const AssetObject& object,
                                   std::vector<AssetGuid>& out) const override {
        const auto& asset = static_cast<const SceneAsset&>(object);
        return scene::CollectSceneReferences(asset.scene, scene::StandardComponentRegistry(), out);
    }
};

} // namespace

std::shared_ptr<const ITypeSerializer> MakeSceneAssetSerializer() {
    return std::make_shared<SceneAssetSerializer>();
}

} // namespace parcel::assets::detail
