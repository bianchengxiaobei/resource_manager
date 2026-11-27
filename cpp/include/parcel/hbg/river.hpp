#pragma once

#include "parcel/hbg/asset_materializer.hpp"

#include "render/scene/render_scene.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace parcel_hbg {

struct RiverParticleSettings {
    bool enabled = true;
    std::uint32_t max_particles = 512;
    float speed_scale = 800.0f;
    float screen_radius = 15.0f;
    float fade_in_seconds = 2.0f;
    float active_seconds = 30.0f;
    float fade_out_seconds = 2.0f;
    std::uint32_t texture_size = 64;
    std::uint32_t seed = 171717U;
};

struct RiverView {
    hbg::Mat4 view_proj = hbg::Mat4::identity();
    hbg::Mat4 inv_view_proj = hbg::Mat4::identity();
    hbg::Vec3 camera_position{};
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;

    [[nodiscard]] bool valid() const noexcept { return viewport_width > 0 && viewport_height > 0; }
};

struct RiverSettings {
    float length = 1600.0f;
    float width = 64.0f;
    float amplitude = 180.0f;
    float waves = 0.75f;
    std::uint32_t segments = 320;
    std::uint32_t cross_segments = 12;
    float base_height = 0.05f;
    float uv_length = 180.0f;
    std::string material_name = "M_River";
    hbg::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
    hbg::Vec4 material_tint{0.22f, 0.52f, 0.78f, 0.72f};
    float metallic = 0.0f;
    float roughness = 0.35f;
    bool deform = true;
    bool fill_bed = true;
    std::string terrain_mesh_name = "RiverTerrain";
    float bed_depth = 1.5f;
    float bed_width_factor = 0.9f;
    float bank_width_factor = 2.2f;
    std::uint32_t texture_size = 256;
    std::uint32_t wave_tile_size = 256;
    std::uint32_t wave_time_loop = 8;
    float wave_time_step = 5.0f;
    float wave_slope = 0.18f;
    float wave_slope_factor = 10.0f;
    float wave_length = 40.0f;
    RiverParticleSettings particles;
};

[[nodiscard]] float RiverLifecycleFade(float age, float fade_in, float active, float fade_out);

struct RiverStats {
    bool active = false;
    std::uint32_t mesh_index = UINT32_MAX;
    std::uint32_t object_index = UINT32_MAX;
    std::uint32_t material_index = UINT32_MAX;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    std::uint32_t flow_samples = 0;
    float min_speed = 0.0f;
    float max_speed = 0.0f;
    bool terrain_deformed = false;
    std::string terrain_mesh_name;
    std::uint32_t terrain_vertex_count = 0;
    std::uint32_t terrain_changed_vertex_count = 0;
    float terrain_min_delta = 0.0f;
    float terrain_max_delta = 0.0f;
    bool textures_generated = false;
    std::uint32_t texture_size = 0;
    std::uint32_t base_color_image_index = UINT32_MAX;
    std::uint32_t normal_image_index = UINT32_MAX;
    std::uint32_t normal_frame_count = 1;
    std::uint32_t normal_frame_index = 0;
    float normal_frame_time = 0.0f;
    float normal_frame_step = 5.0f;
    bool foam_generated = false;
    std::uint32_t foam_particle_count = 0;
    std::uint32_t foam_mesh_index = UINT32_MAX;
    std::uint32_t foam_object_index = UINT32_MAX;
    std::uint32_t foam_material_index = UINT32_MAX;
    std::uint32_t foam_vertex_count = 0;
    std::uint32_t foam_index_count = 0;
    std::uint32_t foam_texture_size = 0;
    std::uint32_t foam_texture_image_index = UINT32_MAX;
    float foam_min_fade = 0.0f;
    float foam_max_fade = 0.0f;
    bool foam_dynamic = false;
    std::uint32_t foam_morph_target_index = UINT32_MAX;
    std::uint32_t foam_updates = 0;
    float foam_time = 0.0f;
    float foam_max_displacement = 0.0f;
    bool foam_screen_space = false;
    std::uint32_t foam_screen_particle_count = 0;
};

class RiverScene {
  public:
    RiverScene();
    ~RiverScene();

    RiverScene(const RiverScene&) = delete;
    RiverScene& operator=(const RiverScene&) = delete;
    RiverScene(RiverScene&&) noexcept;
    RiverScene& operator=(RiverScene&&) noexcept;

    [[nodiscard]] bool Apply(hbg::render::RenderScene& scene, std::string_view sidecar_json,
                             std::string* error = nullptr,
                             std::vector<MaterializedTexture>* material_images = nullptr);
    [[nodiscard]] bool Apply(hbg::render::RenderScene& scene, const RiverSettings& settings,
                             std::string* error = nullptr,
                             std::vector<MaterializedTexture>* material_images = nullptr);
    [[nodiscard]] bool Update(hbg::render::RenderScene& scene, float delta_seconds,
                              const RiverView* view = nullptr);
    void Close();

    [[nodiscard]] bool active() const noexcept { return stats_.active; }
    [[nodiscard]] const RiverStats& stats() const noexcept { return stats_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RiverStats stats_;
};

} // namespace parcel_hbg
