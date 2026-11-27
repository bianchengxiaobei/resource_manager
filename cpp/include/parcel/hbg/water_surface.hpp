#pragma once

#include "render/scene/render_scene.h"

#include <cstdint>
#include <memory>
#include <string>

namespace parcel_hbg {

struct WaterSurfaceSettings {
    float tile_size = 5488.0f;
    float amplitude = 1.0f;
    float update_hz = 12.0f;
    float sphere_radius = 0.0f;
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
};

struct WaterSurfaceStats {
    bool active = false;
    std::uint32_t object_index = UINT32_MAX;
    std::uint32_t mesh_index = UINT32_MAX;
    std::uint32_t vertex_count = 0;
    std::uint32_t fft_updates = 0;
    std::uint32_t morph_updates = 0;
    bool screen_grid = false;
    std::uint32_t grid_width = 0;
    std::uint32_t grid_height = 0;
    float base_height = 0.0f;
    float min_height = 0.0f;
    float max_height = 0.0f;
    float max_displacement = 0.0f;
};

class WaterSurface {
  public:
    WaterSurface();
    ~WaterSurface();

    WaterSurface(const WaterSurface&) = delete;
    WaterSurface& operator=(const WaterSurface&) = delete;
    WaterSurface(WaterSurface&&) noexcept;
    WaterSurface& operator=(WaterSurface&&) noexcept;

    [[nodiscard]] bool Bind(hbg::render::RenderScene& scene, const WaterSurfaceSettings& settings,
                            std::string* error = nullptr);
    [[nodiscard]] bool Update(hbg::render::RenderScene& scene, float time_seconds,
                              const hbg::Mat4* inv_view_proj = nullptr,
                              const hbg::Vec3* camera_position = nullptr);
    void Close();

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const WaterSurfaceStats& stats() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace parcel_hbg
