#pragma once

#include "render/scene/render_scene.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace parcel_hbg {

struct TerrainLodSettings {
    float world_size = 50000.0f;
    float origin_x = -25000.0f;
    float origin_z = -25000.0f;
    float zmin = 0.0f;
    float zmax = 5000.0f;
    float ground_height = 0.0f;
    float skirt_depth = 0.0f;
    float split_factor = 2.0f;
    float viewport_height = 1080.0f;
    float fov_y_radians = 0.9599311f;
    std::uint32_t max_level = 4;
    bool split_invisible_quads = false;
    bool horizon_culling = true;
    bool water_fft = false;
    float water_fft_tile_size = 5488.0f;
    float water_fft_amplitude = 1.0f;
    float water_fft_update_hz = 12.0f;
    float water_sphere_radius = 0.0f;
    float vegetation_max_distance = 5000.0f;
    float vegetation_tile_size = 500.0f;
    bool shared_geometry = false;
    std::uint32_t terrain_coarse_grid = 33;
    std::uint32_t terrain_fine_grid = 65;
    std::uint32_t terrain_fine_from_level = 3;
    std::string terrain_coarse_mesh;
    std::string terrain_fine_mesh;
    std::string name_prefix = "TerrainLOD_";
};

struct TerrainHeightmapStats {
    bool loaded = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float height_scale = 1.0f;
    std::uint32_t hits = 0;
    std::uint32_t misses = 0;
    std::uint32_t loaded_tiles = 0;
    std::uint32_t evictions = 0;
    float ground_height = 0.0f;
    bool ground_sampled = false;
};

struct TerrainLodStats {
    std::uint32_t node_count = 0;
    std::uint32_t leaf_count = 0;
    std::uint32_t visible_leaf_count = 0;
    std::uint32_t horizon_culled_leaf_count = 0;
    std::uint32_t max_depth = 0;
    std::uint32_t bound_object_count = 0;
    std::uint32_t visible_object_count = 0;
    bool horizon_active = false;
    bool shared_geometry = false;
    std::uint32_t shared_mesh_count = 0;
    std::uint32_t shared_vertex_count = 0;
    std::uint32_t shared_triangle_count = 0;
};

class TerrainLodController {
  public:
    TerrainLodController();
    ~TerrainLodController();

    TerrainLodController(const TerrainLodController&) = delete;
    TerrainLodController& operator=(const TerrainLodController&) = delete;
    TerrainLodController(TerrainLodController&&) noexcept;
    TerrainLodController& operator=(TerrainLodController&&) noexcept;

    [[nodiscard]] bool Load(std::string_view sidecar_json, hbg::render::RenderScene& scene,
                            std::string* error = nullptr);
    [[nodiscard]] bool Load(std::string_view sidecar_json,
                            std::span<const std::byte> heightmap_data,
                            hbg::render::RenderScene& scene, std::string* error = nullptr);
    void Update(hbg::render::RenderScene& scene, const hbg::Vec3& camera_position,
                const std::array<hbg::Vec4, 6>* frustum_planes = nullptr,
                const hbg::Vec3* camera_forward = nullptr);
    void Restore(hbg::render::RenderScene& scene);

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool water_only() const noexcept;
    [[nodiscard]] const TerrainLodSettings& settings() const noexcept;
    [[nodiscard]] const TerrainLodStats& stats() const noexcept;
    [[nodiscard]] const TerrainHeightmapStats& heightmap_stats() const noexcept;
    [[nodiscard]] const std::string& heightmap_error() const noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace parcel_hbg
