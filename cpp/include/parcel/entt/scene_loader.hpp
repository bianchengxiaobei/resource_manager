#pragma once

#include "parcel/assets/scene_asset.hpp"
#include "parcel/core/result.hpp"
#include "parcel/scene/component_codec.hpp"
#include "parcel/scene/scene_loader.hpp"

#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parcel_entt {

using parcel::core::Result;
using parcel::scene::ComponentTypeId;
using parcel::scene::SceneData;
using parcel::scene::SceneEntityId;

struct SceneEntityComponent {
    SceneEntityId id;
    std::string name;
    std::uint32_t layer = 0;
    std::uint32_t flags = 0;
    std::uint32_t scene_order = 0;
};

struct ParentEntityComponent {
    SceneEntityId id;
    ::entt::entity entity = ::entt::null;
};

class EnttComponentRegistry {
  public:
    using EmplaceFn = Result<void> (*)(::entt::registry&, ::entt::entity,
                                       std::span<const std::byte>);

    struct Entry {
        ComponentTypeId id = 0;
        std::string name;
        std::uint32_t schema_version = 0;
        EmplaceFn emplace = nullptr;
    };

    [[nodiscard]] Result<void> Register(ComponentTypeId id, std::string_view name,
                                        std::uint32_t schema_version, EmplaceFn emplace);
    [[nodiscard]] const Entry* Find(ComponentTypeId id) const noexcept;
    [[nodiscard]] const Entry* Find(std::string_view name) const noexcept;
    [[nodiscard]] std::span<const Entry> Types() const noexcept { return types_; }

    template <typename T> [[nodiscard]] Result<void> Register() {
        return Register(parcel::scene::ComponentCodec<T>::type_id,
                        std::string(parcel::scene::ComponentCodec<T>::type_name),
                        parcel::scene::ComponentCodec<T>::schema_version, &EmplaceComponent<T>);
    }

    template <typename T>
    static Result<void> EmplaceComponent(::entt::registry& registry, ::entt::entity entity,
                                         std::span<const std::byte> payload) {
        auto decoded = parcel::scene::ComponentCodec<T>::Decode(payload);
        if (!decoded) {
            return std::move(decoded).TakeError();
        }
        registry.emplace_or_replace<T>(entity, std::move(decoded).Value());
        return {};
    }

    [[nodiscard]] Result<void> Emplace(::entt::registry& registry, ::entt::entity entity,
                                       ComponentTypeId type, std::uint32_t schema_version,
                                       std::span<const std::byte> payload) const;

  private:
    std::vector<Entry> types_;
};

[[nodiscard]] const EnttComponentRegistry& StandardEnttComponentRegistry();

class EnttSceneLoader final : public parcel::scene::ISceneLoader {
  public:
    EnttSceneLoader() = default;
    explicit EnttSceneLoader(const EnttComponentRegistry* components) : components_(components) {}
    explicit EnttSceneLoader(const EnttComponentRegistry& components) : components_(&components) {}

    [[nodiscard]] Result<parcel::scene::SceneInstance> Instantiate(const SceneData& scene,
                                                                   void* registry) override;
    [[nodiscard]] Result<parcel::scene::SceneInstance>
    Instantiate(const parcel::assets::SceneAsset& asset, void* registry) {
        return Instantiate(asset.scene, registry);
    }
    [[nodiscard]] Result<void> Destroy(parcel::scene::SceneInstance& instance) override;

  private:
    [[nodiscard]] const EnttComponentRegistry& Components() const noexcept;

    const EnttComponentRegistry* components_ = nullptr;
};

} // namespace parcel_entt
