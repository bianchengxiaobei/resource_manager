#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/math.hpp"

#include <cstdint>
#include <string>

namespace parcel::assets {

enum class MaterialType : std::uint8_t { Lit, Unlit };
enum class AlphaMode : std::uint8_t { Opaque, Mask, Blend };

enum MaterialFeature : std::uint32_t {
    MaterialFeatureNone = 0,
    MaterialFeatureClearCoat = 1U << 0,
    MaterialFeatureTransmission = 1U << 1,
    MaterialFeatureIor = 1U << 2,
    MaterialFeatureIridescence = 1U << 3,
    MaterialFeatureAnisotropy = 1U << 4,
    MaterialFeatureEmissiveStrength = 1U << 5,
    MaterialFeatureSubsurface = 1U << 6,
};

enum class TextureWrap : std::uint8_t { Repeat, Clamp, Mirror };
enum class TextureFilter : std::uint8_t { Linear, Nearest };
enum class TextureMipFilter : std::uint8_t { Linear, Nearest };

struct TextureSampler {
    TextureWrap wrap_s = TextureWrap::Clamp;
    TextureWrap wrap_t = TextureWrap::Clamp;
    TextureFilter min_filter = TextureFilter::Linear;
    TextureFilter mag_filter = TextureFilter::Linear;
    TextureMipFilter mip_filter = TextureMipFilter::Linear;

    bool operator==(const TextureSampler&) const = default;
};

struct TextureRef {
    AssetGuid texture;
    std::int32_t uv_set = 0;
    TextureSampler sampler;

    [[nodiscard]] bool IsValid() const noexcept { return !texture.IsZero(); }
};

class MaterialAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kMaterialAssetType; }

    std::string name;
    MaterialType type = MaterialType::Lit;
    AlphaMode alpha_mode = AlphaMode::Opaque;
    bool double_sided = false;
    std::uint32_t features = MaterialFeatureNone;
    std::int32_t render_queue = 2000;

    Vec4 base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 1.0f;
    float roughness = 1.0f;
    Vec3 emissive_factor{};
    float emissive_strength = 1.0f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    float alpha_cutoff = 0.5f;

    TextureRef base_color_texture;
    TextureRef metallic_roughness_texture;
    TextureRef normal_texture;
    TextureRef occlusion_texture;
    TextureRef emissive_texture;

    std::string diffusion_profile;
    bool mask_map_packed = true;

    float clear_coat = 0.0f;
    float clear_coat_roughness = 0.03f;
    float clear_coat_normal_scale = 1.0f;
    float transmission = 0.0f;
    float thickness = 0.0f;
    float ior = 1.5f;
    float iridescence = 0.0f;
    float iridescence_ior = 1.3f;
    float iridescence_thickness = 0.0f;
    float anisotropy = 0.0f;
    float anisotropy_rotation = 0.0f;
    float subsurface_mask = 0.0f;
};

} // namespace parcel::assets
