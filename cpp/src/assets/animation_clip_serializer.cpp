#include "parcel/assets/animation_clip_asset.hpp"
#include "parcel/assets/serializers.hpp"
#include "parcel/serialize/math_io.hpp"
#include "serializer_util.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace parcel::assets::detail {
namespace {

class AnimationClipAssetSerializer final : public ITypeSerializer {
  public:
    TypeId Type() const noexcept override { return kAnimationClipAssetType; }
    std::string_view TypeName() const noexcept override { return "AnimationClipAsset"; }
    std::uint32_t Version() const noexcept override { return 1; }

    Result<std::shared_ptr<AssetObject>> Create() const override {
        return std::static_pointer_cast<AssetObject>(std::make_shared<AnimationClipAsset>());
    }

    Result<void> Serialize(const AssetObject& object, BinaryWriter& writer,
                           IAssetWriteContext&) const override {
        const auto& asset = static_cast<const AnimationClipAsset&>(object);
        PARCEL_TRY(ValidateAnimationClip(asset));
        PARCEL_TRY(writer.WriteString(asset.name));
        PARCEL_TRY(writer.WriteF32(asset.duration));
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(asset.channels.size())));
        for (const AnimationChannel& channel : asset.channels) {
            PARCEL_TRY(writer.WriteU64(channel.target.value));
            PARCEL_TRY(WriteEnum(writer, channel.path));
            PARCEL_TRY(WriteEnum(writer, channel.interpolation));
            PARCEL_TRY(writer.WriteU32(channel.target_index));
            PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(channel.times.size())));
            for (float time : channel.times) {
                PARCEL_TRY(writer.WriteF32(time));
            }
            for (const Vec4& value : channel.values) {
                PARCEL_TRY(serialize::WriteVec4(writer, value));
            }
            const bool has_tangents = !channel.in_tangents.empty();
            PARCEL_TRY(writer.WriteU8(has_tangents ? 1 : 0));
            if (has_tangents) {
                for (const Vec4& value : channel.in_tangents) {
                    PARCEL_TRY(serialize::WriteVec4(writer, value));
                }
                for (const Vec4& value : channel.out_tangents) {
                    PARCEL_TRY(serialize::WriteVec4(writer, value));
                }
            }
        }
        return {};
    }

    Result<std::shared_ptr<AssetObject>> Deserialize(BinaryReader& reader,
                                                     IAssetReadContext&) const override {
        auto asset = std::make_shared<AnimationClipAsset>();
        PARCEL_TRY_DECL(std::string_view, name, reader.ReadString());
        asset->name.assign(name);
        PARCEL_TRY_ASSIGN(asset->duration, reader.ReadF32());
        PARCEL_TRY_DECL(std::uint32_t, channel_count,
                        ReadCount(reader, kMaxAnimationChannels, 19, "animation channel"));
        if (channel_count == 0) {
            return MakeError(ErrorCode::Corrupt, "animation clip has no channels");
        }
        asset->channels.resize(channel_count);
        for (AnimationChannel& channel : asset->channels) {
            PARCEL_TRY_ASSIGN(channel.target.value, reader.ReadU64());
            PARCEL_TRY_ASSIGN(channel.path, ReadEnum<AnimationPath>(reader, 3));
            PARCEL_TRY_ASSIGN(channel.interpolation, ReadEnum<AnimationInterpolation>(reader, 2));
            PARCEL_TRY_ASSIGN(channel.target_index, reader.ReadU32());
            PARCEL_TRY_DECL(std::uint32_t, key_count,
                            ReadCount(reader, kMaxAnimationKeys, 20, "animation key"));
            if (key_count == 0) {
                return MakeError(ErrorCode::Corrupt, "animation channel has no keys");
            }
            channel.times.resize(key_count);
            for (float& time : channel.times) {
                PARCEL_TRY_ASSIGN(time, reader.ReadF32());
            }
            channel.values.resize(key_count);
            for (Vec4& value : channel.values) {
                PARCEL_TRY_ASSIGN(value, serialize::ReadVec4(reader));
            }
            PARCEL_TRY_DECL(std::uint8_t, has_tangents, reader.ReadU8());
            if (has_tangents > 1) {
                return MakeError(ErrorCode::Corrupt, "animation tangent flag is invalid");
            }
            if (has_tangents != 0) {
                channel.in_tangents.resize(key_count);
                for (Vec4& value : channel.in_tangents) {
                    PARCEL_TRY_ASSIGN(value, serialize::ReadVec4(reader));
                }
                channel.out_tangents.resize(key_count);
                for (Vec4& value : channel.out_tangents) {
                    PARCEL_TRY_ASSIGN(value, serialize::ReadVec4(reader));
                }
            }
        }
        PARCEL_TRY(Finish(reader, "animation clip metadata"));
        PARCEL_TRY(ValidateAnimationClip(*asset));
        return std::static_pointer_cast<AssetObject>(asset);
    }

    Result<void> CollectReferences(const AssetObject&, std::vector<AssetGuid>&) const override {
        return {};
    }
};

} // namespace

std::shared_ptr<const ITypeSerializer> MakeAnimationClipAssetSerializer() {
    return std::make_shared<AnimationClipAssetSerializer>();
}

} // namespace parcel::assets::detail
