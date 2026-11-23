#include "parcel/assets/ies_profile_asset.hpp"
#include "parcel/assets/serializers.hpp"
#include "serializer_util.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace parcel::assets::detail {
namespace {

class IesProfileAssetSerializer final : public ITypeSerializer {
  public:
    TypeId Type() const noexcept override { return kIesProfileAssetType; }
    std::string_view TypeName() const noexcept override { return "IesProfileAsset"; }
    std::uint32_t Version() const noexcept override { return 1; }

    Result<std::shared_ptr<AssetObject>> Create() const override {
        return std::static_pointer_cast<AssetObject>(std::make_shared<IesProfileAsset>());
    }

    Result<void> Serialize(const AssetObject& object, BinaryWriter& writer,
                           IAssetWriteContext&) const override {
        const auto& asset = static_cast<const IesProfileAsset&>(object);
        PARCEL_TRY(ValidateIesProfile(asset));
        PARCEL_TRY(writer.WriteString(asset.name));
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(asset.vertical_angles.size())));
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(asset.horizontal_angles.size())));
        for (float angle : asset.vertical_angles) {
            PARCEL_TRY(writer.WriteF32(angle));
        }
        for (float angle : asset.horizontal_angles) {
            PARCEL_TRY(writer.WriteF32(angle));
        }
        for (float value : asset.values) {
            PARCEL_TRY(writer.WriteF32(value));
        }
        return {};
    }

    Result<std::shared_ptr<AssetObject>> Deserialize(BinaryReader& reader,
                                                     IAssetReadContext&) const override {
        auto asset = std::make_shared<IesProfileAsset>();
        PARCEL_TRY_DECL(std::string_view, name, reader.ReadString());
        asset->name.assign(name);
        PARCEL_TRY_DECL(std::uint32_t, vertical_count,
                        ReadCount(reader, kMaxIesAngles, 4, "IES vertical angle"));
        PARCEL_TRY_DECL(std::uint32_t, horizontal_count,
                        ReadCount(reader, kMaxIesAngles, 4, "IES horizontal angle"));
        if (vertical_count < 2 || horizontal_count < 1) {
            return MakeError(ErrorCode::Corrupt, "IES angle count is out of range");
        }

        asset->vertical_angles.resize(vertical_count);
        for (float& angle : asset->vertical_angles) {
            PARCEL_TRY_ASSIGN(angle, reader.ReadF32());
        }
        asset->horizontal_angles.resize(horizontal_count);
        for (float& angle : asset->horizontal_angles) {
            PARCEL_TRY_ASSIGN(angle, reader.ReadF32());
        }

        const std::uint64_t value_count =
            static_cast<std::uint64_t>(vertical_count) * horizontal_count;
        if (value_count > std::numeric_limits<std::uint32_t>::max() ||
            value_count > reader.Remaining() / sizeof(float)) {
            return MakeError(ErrorCode::Corrupt, "IES value count exceeds metadata");
        }
        asset->values.resize(static_cast<std::size_t>(value_count));
        for (float& value : asset->values) {
            PARCEL_TRY_ASSIGN(value, reader.ReadF32());
        }
        PARCEL_TRY(Finish(reader, "IES metadata"));
        PARCEL_TRY(ValidateIesProfile(*asset));
        return std::static_pointer_cast<AssetObject>(asset);
    }

    Result<void> CollectReferences(const AssetObject&, std::vector<AssetGuid>&) const override {
        return {};
    }
};

} // namespace

std::shared_ptr<const ITypeSerializer> MakeIesProfileAssetSerializer() {
    return std::make_shared<IesProfileAssetSerializer>();
}

} // namespace parcel::assets::detail
