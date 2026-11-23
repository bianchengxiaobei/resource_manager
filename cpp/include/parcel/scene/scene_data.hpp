#pragma once

#include "parcel/core/guid.hpp"
#include "parcel/core/result.hpp"
#include "parcel/scene/component_codec.hpp"
#include "parcel/scene/components.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::scene {

using core::AssetGuid;
using core::Result;

inline constexpr char kSceneMagic[4] = {'P', 'R', 'C', 'S'};
inline constexpr std::uint16_t kSceneVersion = 1;
inline constexpr std::uint16_t kSceneHeaderSize = 128;
inline constexpr std::uint32_t kMaxSceneEntities = 10U * 1000U * 1000U;
inline constexpr std::uint32_t kMaxSceneComponents = 1U * 1000U * 1000U;

struct SceneEntityRecord {
    SceneEntityId id;
    SceneEntityId parent;
    std::string name;
    TransformData transform;
    std::uint32_t layer = 0;
    std::uint32_t flags = 0;
};

struct ComponentChunk {
    ComponentTypeId type = 0;
    std::uint32_t schema_version = kStandardComponentSchemaVersion;
    std::vector<SceneEntityId> owners;
    std::vector<std::vector<std::byte>> payloads;
};

struct SceneData {
    std::vector<SceneEntityRecord> entities;
    std::vector<ComponentChunk> components;
    AssetGuid pipeline_settings;
    AssetGuid frame_settings;
    AssetGuid post_process_profile;
    AssetGuid shadow_settings;
    AssetGuid volumetric_fog_profile;

    [[nodiscard]] const SceneEntityRecord* FindEntity(SceneEntityId id) const noexcept;
    [[nodiscard]] SceneEntityRecord* FindEntity(SceneEntityId id) noexcept;
    [[nodiscard]] const ComponentChunk* FindComponent(ComponentTypeId type) const noexcept;
    [[nodiscard]] ComponentChunk* FindComponent(ComponentTypeId type) noexcept;
};

using CollectComponentReferencesFn = Result<void> (*)(std::span<const std::byte> payload,
                                                      std::vector<AssetGuid>& out);

struct ComponentTypeInfo {
    ComponentTypeId id = 0;
    std::string name;
    std::uint32_t schema_version = kStandardComponentSchemaVersion;
    CollectComponentReferencesFn collect_references = nullptr;
};

class ComponentRegistry {
  public:
    [[nodiscard]] Result<void> Register(ComponentTypeInfo info);
    [[nodiscard]] const ComponentTypeInfo* Find(ComponentTypeId id) const noexcept;
    [[nodiscard]] const ComponentTypeInfo* Find(std::string_view name) const noexcept;
    [[nodiscard]] std::span<const ComponentTypeInfo> Types() const noexcept { return types_; }

  private:
    std::vector<ComponentTypeInfo> types_;
};

template <typename T> Result<void> RegisterComponent(ComponentRegistry& registry) {
    return registry.Register(ComponentTypeInfo{
        ComponentCodec<T>::type_id,
        std::string(ComponentCodec<T>::type_name),
        ComponentCodec<T>::schema_version,
        nullptr,
    });
}

[[nodiscard]] const ComponentRegistry& StandardComponentRegistry();

[[nodiscard]] Result<void> ValidateScene(const SceneData& scene, const ComponentRegistry& registry);
[[nodiscard]] Result<void> ValidateEntityReferences(const SceneData& scene,
                                                    const ComponentRegistry& registry);
[[nodiscard]] Result<void> CollectSceneReferences(const SceneData& scene,
                                                  const ComponentRegistry& registry,
                                                  std::vector<AssetGuid>& out);

[[nodiscard]] Result<std::vector<std::byte>> EncodeScene(const SceneData& scene,
                                                         const ComponentRegistry& registry);
[[nodiscard]] Result<SceneData> DecodeScene(std::span<const std::byte> data,
                                            const ComponentRegistry& registry);

} // namespace parcel::scene
