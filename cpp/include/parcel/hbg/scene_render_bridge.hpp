#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/core/result.hpp"
#include "parcel/entt/scene_loader.hpp"
#include "parcel/math.hpp"

#include <entt/entt.hpp>
#include <render/scene/render_scene.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace parcel_hbg {

using parcel::core::AssetGuid;
using parcel::core::Result;

struct CameraState {
    parcel::Vec4 clear_color{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t culling_mask = 0xffffffffU;
    std::int32_t priority = 0;
};

struct LightTextureState {
    int cookie_image = -1;
    int ies_image = -1;
};

class SceneRenderBridge {
  public:
    using AssetResolver =
        std::function<Result<std::shared_ptr<parcel::assets::AssetObject>>(AssetGuid)>;

    SceneRenderBridge() = default;
    explicit SceneRenderBridge(AssetResolver resolver) : resolver_(std::move(resolver)) {}

    void SetResolver(AssetResolver resolver) { resolver_ = std::move(resolver); }
    void MarkDirty() noexcept { dirty_ = true; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

    [[nodiscard]] Result<void> Update(const ::entt::registry& registry,
                                      hbg::render::RenderScene& scene);
    [[nodiscard]] Result<void> UpdateIfDirty(const ::entt::registry& registry,
                                             hbg::render::RenderScene& scene);

    [[nodiscard]] const std::vector<CameraState>& camera_states() const noexcept {
        return camera_states_;
    }
    [[nodiscard]] const std::vector<LightTextureState>& light_texture_states() const noexcept {
        return light_texture_states_;
    }
    [[nodiscard]] const std::vector<AssetGuid>& image_guids() const noexcept {
        return image_guids_;
    }

  private:
    [[nodiscard]] Result<int> ResolveImage(AssetGuid guid, hbg::render::RenderScene& scene);
    [[nodiscard]] Result<std::shared_ptr<parcel::assets::AssetObject>>
    ResolveAsset(AssetGuid guid) const;

    AssetResolver resolver_;
    std::unordered_map<AssetGuid, int, parcel::core::Guid128Hash> image_indices_;
    std::vector<AssetGuid> image_guids_;
    std::vector<CameraState> camera_states_;
    std::vector<LightTextureState> light_texture_states_;
    bool dirty_ = true;
};

} // namespace parcel_hbg
