#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/math.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace parcel::assets {

enum class SupportedLitShaderMode : std::uint8_t {
    ForwardOnly = 1U << 0,
    DeferredOnly = 1U << 1,
    Both = (1U << 0) | (1U << 1),
};

enum class ColorBufferFormat : std::uint8_t { R11G11B10F, R16G16B16A16F };
enum class DepthBufferFormat : std::uint8_t { Auto, D24S8, D32F, D16 };
enum class MSAASamples : std::uint8_t { None = 1, MSAA2X = 2, MSAA4X = 4, MSAA8X = 8 };

struct PipelineSettings {
    SupportedLitShaderMode supported_lit_shader_mode = SupportedLitShaderMode::ForwardOnly;
    ColorBufferFormat color_buffer_format = ColorBufferFormat::R11G11B10F;
    DepthBufferFormat depth_buffer_format = DepthBufferFormat::Auto;
    MSAASamples msaa_sample_count = MSAASamples::None;

    bool support_shadows = true;
    bool support_shadow_mask = false;
    bool support_gpu_light_list = true;
    bool support_gpu_light_list_buffers = true;
    bool support_gpu_light_list_indirect = false;
    bool support_light_list_feature_flags = true;
    bool support_lit_variants = false;
    bool support_lit_variant_tiles = false;
    bool support_cookie_atlas = true;
    std::uint32_t cookie_atlas_size = 1024;
    std::uint32_t light_list_capacity = 64;
    bool support_gpu_shadow_culling = false;
    bool support_gpu_shadow_indirect = false;
    bool support_async_gpu_light_list = false;
    bool support_contact_shadows = false;
    bool support_screen_space_shadows = false;
    bool support_ssr = false;
    bool support_ssr_transparent = false;
    bool support_ssao = false;
    bool support_ssgi = false;
    bool support_subsurface_scattering = false;
    bool support_volumetrics = false;
    bool support_volumetric_clouds = false;
    bool support_motion_vectors = true;
    bool support_taa = false;
    bool support_bloom = false;
    bool support_color_grading = false;
    bool support_depth_of_field = false;
    bool support_motion_blur = false;
    bool support_fxaa = false;
    bool support_exposure_histogram = false;
    bool support_transparent_depth_prepass = false;
    bool support_transparent_depth_postpass = false;
    bool support_low_res_transparent = false;
    bool support_decals = false;
    bool support_decal_layers = false;
    bool support_distortion = false;
    bool support_light_layers = false;
    bool support_water = false;
    bool support_water_decals = false;
    bool support_water_exclusion = false;
    bool support_ray_tracing = false;
    bool support_screen_space_lens_flare = false;
    bool support_data_driven_lens_flare = false;
    bool support_custom_pass = false;
    bool support_variable_rate_shading = false;
    bool support_virtual_texturing = false;
    bool support_probe_volume = false;
    bool support_high_quality_line_rendering = false;
    bool support_compute_thickness = false;
    bool support_async_compute = false;
    bool rendering_layer_mask_buffer = false;
};

class PipelineSettingsAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kPipelineSettingsAssetType; }
    PipelineSettings settings;
};

enum class LitShaderMode : std::uint8_t { Forward, Deferred };
enum class LODBiasMode : std::uint8_t {
    FromQualitySettings,
    ScaleQualitySettings,
    OverrideQualitySettings,
};
enum class MaximumLODLevelMode : std::uint8_t {
    FromQualitySettings,
    OffsetQualitySettings,
    OverrideQualitySettings,
};
enum class SssQualityMode : std::uint8_t { FromQualitySettings, OverrideQualitySettings };
enum class MSAAMode : std::uint8_t {
    FromHDRPAsset = 0,
    None = 1,
    MSAA2X = 2,
    MSAA4X = 4,
    MSAA8X = 8
};
enum class MaterialQuality : std::uint8_t { Low = 1U << 0, Medium = 1U << 1, High = 1U << 2 };

enum class FrameSettingsField : std::int16_t {
    None = -1,
    LitShaderMode = 0,
    OpaqueObjects = 2,
    TransparentObjects = 3,
    MSAAMode = 4,
    Decals = 12,
    Postprocess = 15,
    MotionVectors = 10,
    ShadowMaps = 20,
    ContactShadows = 21,
    SSR = 23,
    SSAO = 24,
    AtmosphericScattering = 27,
    Volumetrics = 28,
    ReprojectionForVolumetrics = 29,
    LightLayers = 30,
    ExposureControl = 32,
    ReflectionProbe = 33,
    ScreenSpaceShadows = 34,
    SubsurfaceScattering = 46,
    LODBias = 61,
    MaximumLODLevel = 63,
    MaterialQualityLevel = 66,
    VirtualTexturing = 68,
    VolumetricClouds = 79,
    DepthOfField = 81,
    MotionBlur = 82,
    Bloom = 84,
    Vignette = 87,
    ColorGrading = 88,
    Antialiasing = 91,
    Tonemapping = 93,
    TransparentSSR = 94,
    SSGI = 95,
    DecalLayers = 96,
    Water = 99,
};

struct FrameSettings {
    std::array<std::uint64_t, 2> bit_data{};

    float lod_bias = 1.0f;
    LODBiasMode lod_bias_mode = LODBiasMode::FromQualitySettings;
    std::int32_t lod_bias_quality_level = 0;
    std::int32_t maximum_lod_level = 0;
    MaximumLODLevelMode maximum_lod_level_mode = MaximumLODLevelMode::FromQualitySettings;
    std::int32_t maximum_lod_level_quality_level = 0;
    SssQualityMode sss_quality_mode = SssQualityMode::FromQualitySettings;
    std::int32_t sss_quality_level = 0;
    std::int32_t sss_custom_sample_budget = 0;
    std::int32_t sss_custom_downsample_steps = 0;
    MSAAMode msaa_mode = MSAAMode::None;
    MaterialQuality material_quality = MaterialQuality::High;

    [[nodiscard]] bool IsEnabled(FrameSettingsField field) const noexcept;
    void SetEnabled(FrameSettingsField field, bool value = true) noexcept;
};

class FrameSettingsAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kFrameSettingsAssetType; }
    FrameSettings settings;
};

enum class ExposureMode : std::uint8_t { Fixed, Automatic, Histogram };
enum class TonemappingMode : std::uint8_t { None, Neutral, ACES };
enum class MeteringMode : std::uint8_t { Average, CenterWeighted };
enum class AntialiasingMode : std::uint8_t { None, Taa, Fxaa };

struct BloomSettings {
    bool enabled = false;
    float threshold = 0.0f;
    float intensity = 0.0f;
    float scatter = 0.7f;
    Vec3 tint{1.0f, 1.0f, 1.0f};
    bool high_quality_prefiltering = false;
    bool high_quality_filtering = true;
    std::uint32_t resolution = 2;
};

struct ColorGradingSettings {
    bool enabled = false;
    float post_exposure = 0.0f;
    float contrast = 0.0f;
    Vec3 color_filter{1.0f, 1.0f, 1.0f};
    float hue_shift = 0.0f;
    float saturation = 0.0f;
    float temperature = 0.0f;
    float tint = 0.0f;
    Vec3 channel_mixer_red{1.0f, 0.0f, 0.0f};
    Vec3 channel_mixer_green{0.0f, 1.0f, 0.0f};
    Vec3 channel_mixer_blue{0.0f, 0.0f, 1.0f};
    Vec3 lift{};
    Vec3 gamma{1.0f, 1.0f, 1.0f};
    Vec3 gain{1.0f, 1.0f, 1.0f};
    Vec3 shadows{1.0f, 1.0f, 1.0f};
    Vec3 midtones{1.0f, 1.0f, 1.0f};
    Vec3 highlights{1.0f, 1.0f, 1.0f};
    Vec4 shadows_midtones_highlights_limits{0.0f, 0.5f, 0.5f, 1.0f};
    Vec3 split_shadows{0.5f, 0.5f, 0.5f};
    Vec3 split_highlights{0.5f, 0.5f, 0.5f};
    float split_balance = 0.0f;
};

struct VignetteSettings {
    Vec2 center{0.5f, 0.5f};
    float intensity = 0.0f;
    float smoothness = 0.2f;
    float roundness = 1.0f;
    bool rounded = false;
    Vec3 color{};
    float opacity = 1.0f;
};

struct TaaSettings {
    bool enabled = false;
    std::uint32_t quality = 3;
    float motion_vector_rejection = 0.1f;
    float sharpening = 0.0f;
    float jitter_scale = 1.0f;
};

struct DepthOfFieldSettings {
    bool enabled = false;
    float focus_distance = 10.0f;
    float near_focus_start = 0.0f;
    float near_focus_end = 4.0f;
    float far_focus_start = 10.0f;
    float far_focus_end = 20.0f;
    float near_max_blur = 4.0f;
    float far_max_blur = 8.0f;
    std::uint32_t resolution = 2;
    bool high_quality = false;
};

struct MotionBlurSettings {
    bool enabled = false;
    float intensity = 0.5f;
    float maximum_velocity = 200.0f;
    float minimum_velocity = 2.0f;
    bool camera_motion_blur = true;
    std::uint32_t sample_count = 8;
    float depth_comparison_extent = 1.0f;
};

struct FxaaSettings {
    bool enabled = false;
};

struct PostProcessSettings {
    ExposureMode exposure_mode = ExposureMode::Fixed;
    float fixed_ev100 = -1.2630344f;
    float compensation = 0.0f;
    float limit_min = -1.0f;
    float limit_max = 14.0f;
    float debug_exposure = 0.0f;
    TonemappingMode tonemapping = TonemappingMode::ACES;
    MeteringMode metering = MeteringMode::CenterWeighted;
    float adaptation_speed_dark_to_light = 1.0f;
    float adaptation_speed_light_to_dark = 1.0f;
    float histogram_min_percentile = 0.40f;
    float histogram_max_percentile = 0.90f;
    Vec3 procedural_center{0.5f, 0.5f, 0.0f};
    Vec3 procedural_radii{0.5f, 0.5f, 0.0f};
    float procedural_softness = 0.5f;
    BloomSettings bloom;
    ColorGradingSettings color_grading;
    VignetteSettings vignette;
    TaaSettings taa;
    DepthOfFieldSettings depth_of_field;
    MotionBlurSettings motion_blur;
    FxaaSettings fxaa;
};

struct PostProcessProfile {
    std::string name = "baseline";
    PostProcessSettings settings;
};

class PostProcessProfileAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kPostProcessProfileAssetType; }
    PostProcessProfile profile;
};

enum class ShadowFilter : std::uint8_t {
    Off,
    Pcf3x3,
    Pcf5x5,
    Pcf7x7,
    Pcss,
    Evsm,
    Vsm,
    Msm,
    Ims,
};

struct ContactShadowSettings {
    bool enabled = false;
    bool directional_only = false;
    bool half_resolution = false;
    float length = 0.15f;
    float opacity = 1.0f;
    float distance_scale_factor = 0.5f;
    float min_distance = 0.0f;
    float max_distance = 50.0f;
    float fade_distance = 5.0f;
    float fade_in_distance = 0.0f;
    float ray_bias = 0.2f;
    float thickness_scale = 0.15f;
    std::uint32_t sample_count = 10;
};

struct ShadowSettings {
    float max_distance = 200.0f;
    std::uint32_t cascade_count = 4;
    std::array<float, 4> cascade_splits{0.10f, 0.25f, 0.50f, 1.0f};
    float cascade_fade = 0.10f;
    ShadowFilter directional_filter = ShadowFilter::Pcf7x7;
    ShadowFilter punctual_filter = ShadowFilter::Pcf5x5;
    ShadowFilter area_filter = ShadowFilter::Pcss;
    std::uint32_t directional_resolution = 1024;
    std::uint32_t point_resolution = 256;
    std::uint32_t spot_resolution = 512;
    std::uint32_t area_resolution = 512;
    std::uint32_t atlas_size = 2048;
    std::uint32_t area_atlas_size = 1024;
    std::uint32_t max_requests = 64;
    float normal_bias = 0.2f;
    float slope_bias = 0.5f;
    float shadow_near_plane = 0.1f;
    float directional_light_angle = 1.0f;
    float directional_max_depth_bias = 0.001f;
    std::uint32_t ims_kernel_size = 5;
    float softness = 0.5f;
    std::uint32_t blocker_samples = 16;
    std::uint32_t filter_samples = 24;
    float min_filter_size = 0.01f;
    bool micro_shadows = false;
    float micro_shadow_opacity = 1.0f;
    ContactShadowSettings contact_shadows;
    bool cached_directional_shadows = false;
    bool cached_punctual_shadows = false;
    bool cached_area_shadows = false;
    float shadow_dimmer = 1.0f;
    float shadow_fade_distance = 10000.0f;
    Vec3 shadow_tint{};
};

class ShadowSettingsAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kShadowSettingsAssetType; }
    ShadowSettings settings;
};

struct VolumetricFogSettings {
    bool enabled = false;
    float mean_free_path = 400.0f;
    Vec3 albedo{1.0f, 1.0f, 1.0f};
    float base_height = 0.0f;
    float maximum_height = 50.0f;
    float anisotropy = 0.0f;
    float depth_extent = 64.0f;
    std::uint32_t step_count = 16;
    bool directional_shadows = true;
    float global_light_probe_dimmer = 1.0f;
    float resolution_scale = 0.5f;
    bool temporal = true;
    float temporal_history_weight = 0.85f;
    bool temporal_jitter = true;
    bool punctual_lights = true;
    bool punctual_shadows = true;
    std::uint32_t max_punctual_lights = 8;
    bool local_volumes = true;
};

class VolumetricFogProfileAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kVolumetricFogProfileAssetType; }
    VolumetricFogSettings settings;
};

} // namespace parcel::assets
