#pragma once

#include "parcel/core/guid.hpp"
#include "parcel/core/result.hpp"
#include "parcel/serialize/binary.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace parcel::assets {

using TypeId = std::uint32_t;
using core::AssetGuid;
using core::Result;

inline constexpr TypeId kInvalidTypeId = 0;
inline constexpr TypeId kTextureAssetType = 1;
inline constexpr TypeId kMeshAssetType = 2;
inline constexpr TypeId kMaterialAssetType = 3;
inline constexpr TypeId kShaderLibraryAssetType = 4;
inline constexpr TypeId kRawFileAssetType = 5;
inline constexpr TypeId kTextAssetType = 6;
inline constexpr TypeId kPipelineSettingsAssetType = 7;
inline constexpr TypeId kFrameSettingsAssetType = 8;
inline constexpr TypeId kPostProcessProfileAssetType = 9;
inline constexpr TypeId kShadowSettingsAssetType = 10;
inline constexpr TypeId kVolumetricFogProfileAssetType = 11;
inline constexpr TypeId kSceneAssetType = 12;
inline constexpr TypeId kIesProfileAssetType = 13;
inline constexpr TypeId kAnimationClipAssetType = 14;

class AssetObject {
  public:
    virtual ~AssetObject() = default;
    [[nodiscard]] virtual TypeId Type() const noexcept = 0;
};

class IObjectResolver {
  public:
    virtual ~IObjectResolver() = default;
    [[nodiscard]] virtual Result<std::shared_ptr<AssetObject>> Resolve(AssetGuid guid) = 0;
};

class IAssetWriteContext {
  public:
    virtual ~IAssetWriteContext() = default;
    [[nodiscard]] virtual Result<std::uint64_t> AddBlob(std::span<const std::byte> data) = 0;
};

class IAssetReadContext : public IObjectResolver {
  public:
    [[nodiscard]] virtual Result<std::span<const std::byte>> Blob(std::uint64_t id) const = 0;
};

class ITypeSerializer {
  public:
    virtual ~ITypeSerializer() = default;

    [[nodiscard]] virtual TypeId Type() const noexcept = 0;
    [[nodiscard]] virtual std::string_view TypeName() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t Version() const noexcept = 0;
    [[nodiscard]] virtual Result<std::shared_ptr<AssetObject>> Create() const = 0;
    [[nodiscard]] virtual Result<void> Serialize(const AssetObject& object,
                                                 serialize::BinaryWriter& writer,
                                                 IAssetWriteContext& context) const = 0;
    [[nodiscard]] virtual Result<std::shared_ptr<AssetObject>>
    Deserialize(serialize::BinaryReader& reader, IAssetReadContext& context) const = 0;
    [[nodiscard]] virtual Result<void> CollectReferences(const AssetObject& object,
                                                         std::vector<AssetGuid>& out) const = 0;
};

class TypeRegistry {
  public:
    [[nodiscard]] Result<void> Register(std::shared_ptr<const ITypeSerializer> serializer);
    [[nodiscard]] const ITypeSerializer* Find(TypeId type) const noexcept;
    [[nodiscard]] const ITypeSerializer* Find(std::string_view name) const noexcept;
    [[nodiscard]] std::span<const std::shared_ptr<const ITypeSerializer>>
    Serializers() const noexcept {
        return serializers_;
    }

  private:
    std::vector<std::shared_ptr<const ITypeSerializer>> serializers_;
};

template <typename TSerializer, typename... Args>
[[nodiscard]] Result<void> RegisterAssetSerializer(TypeRegistry& registry, Args&&... args) {
    return registry.Register(std::make_shared<TSerializer>(std::forward<Args>(args)...));
}

} // namespace parcel::assets
