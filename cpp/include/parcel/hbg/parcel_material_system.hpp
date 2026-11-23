#pragma once

#include "parcel/core/result.hpp"
#include "parcel/hbg/asset_materializer.hpp"

#include <render/material/material_system.h>
#include <render/scene/render_scene.h>
#include <shader/shader_key.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace parcel_hbg {

using parcel::core::Result;

[[nodiscard]] hbg::gfx::generated::MaterialUniforms
MakeGpuMaterialUniforms(const hbg::asset::Material& material);
[[nodiscard]] std::uint64_t MakeGpuMaterialVariant(const hbg::asset::Material& material);

class ParcelMaterialSystem {
  public:
    ParcelMaterialSystem() = default;
    ~ParcelMaterialSystem() = default;

    ParcelMaterialSystem(const ParcelMaterialSystem&) = delete;
    ParcelMaterialSystem& operator=(const ParcelMaterialSystem&) = delete;

    [[nodiscard]] Result<void> Initialize(hbg::gfx::ResourceManager& resources,
                                          const hbg::render::RenderScene& scene,
                                          std::span<const MaterializedTexture> images);
    void Shutdown(hbg::gfx::ResourceManager& resources);

    [[nodiscard]] bool initialized() const noexcept { return resources_ != nullptr; }
    [[nodiscard]] const std::vector<hbg::render::GpuMesh>& meshes() const noexcept {
        return meshes_;
    }
    [[nodiscard]] const std::vector<hbg::render::GpuMaterial>& materials() const noexcept {
        return materials_;
    }
    [[nodiscard]] const std::vector<hbg::render::GpuDecalTextures>& decals() const noexcept {
        return decals_;
    }
    [[nodiscard]] const hbg::render::MaterialSamplers& samplers() const noexcept {
        return samplers_;
    }

  private:
    [[nodiscard]] hbg::gfx::TextureHandle TextureForImage(std::size_t image_index,
                                                          bool srgb_fallback) const;
    [[nodiscard]] bool CreateFallbackTextures(hbg::gfx::ResourceManager& resources);

    hbg::gfx::ResourceManager* resources_ = nullptr;
    std::vector<hbg::gfx::TextureHandle> image_textures_;
    std::unordered_map<std::uint64_t, hbg::gfx::TextureHandle> material_textures_;
    std::vector<hbg::render::GpuMesh> meshes_;
    std::vector<hbg::render::GpuMaterial> materials_;
    std::vector<hbg::render::GpuDecalTextures> decals_;
    hbg::render::MaterialSamplers samplers_;
    hbg::gfx::TextureHandle white_srgb_;
    hbg::gfx::TextureHandle white_linear_;
    hbg::gfx::TextureHandle normal_flat_;
};

} // namespace parcel_hbg
