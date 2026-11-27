#pragma once

#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/result.hpp"
#include "parcel/hbg/asset_materializer.hpp"

#include <render/scene/render_scene.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace parcel_hbg {

struct VegetationSettings {
    float tile_size = 500.0f;
    float max_distance = 5000.0f;
    float cull_tile_size = 500.0f;
    float lod_distance = 1500.0f;
};

struct VegetationStats {
    bool built = false;
    std::uint32_t instance_count = 0;
    std::uint32_t deciduous_count = 0;
    std::uint32_t conifer_count = 0;
    std::uint32_t mesh_count = 0;
    std::uint32_t object_count = 0;
    std::uint32_t tile_count = 0;
    std::uint32_t submesh_count = 0;
    std::uint32_t triangle_count = 0;
    std::uint32_t texture_count = 0;
};

struct VegetationCullingStats {
    std::uint32_t object_count = 0;
    std::uint32_t tile_count = 0;
    std::uint32_t submesh_count = 0;
    std::uint32_t visible_object_count = 0;
    std::uint32_t visible_tile_count = 0;
    std::uint32_t visible_submesh_count = 0;
    std::uint32_t frustum_culled_object_count = 0;
    std::uint32_t frustum_culled_submesh_count = 0;
    std::uint32_t distance_culled_object_count = 0;
    std::uint32_t distance_culled_submesh_count = 0;
};

inline constexpr std::string_view kVegetationObjectPrefix = "VegetationTree_";
inline constexpr float kVegetationMeshTileSize = 1000.0f;

[[nodiscard]] Result<VegetationStats>
BuildVegetation(std::string_view trees_json,
                const std::array<const parcel::assets::TextureAsset*, 2>& textures,
                hbg::render::RenderScene& scene, std::vector<MaterializedTexture>& material_images,
                const VegetationSettings& settings = {});

class VegetationController {
  public:
    VegetationController();
    ~VegetationController();

    VegetationController(const VegetationController&) = delete;
    VegetationController& operator=(const VegetationController&) = delete;
    VegetationController(VegetationController&&) noexcept;
    VegetationController& operator=(VegetationController&&) noexcept;

    [[nodiscard]] bool Bind(const hbg::render::RenderScene& scene, float tile_size = 500.0f,
                            float lod_distance = 1500.0f);
    void Update(hbg::render::RenderScene& scene, const hbg::Vec3& camera_position,
                const std::array<hbg::Vec4, 6>* frustum_planes, float max_distance);

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const VegetationCullingStats& stats() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace parcel_hbg
