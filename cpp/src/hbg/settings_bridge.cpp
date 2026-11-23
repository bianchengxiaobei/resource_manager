#include "parcel/hbg/settings_bridge.hpp"

#include "parcel/core/error.hpp"

#include <algorithm>
#include <cmath>

namespace parcel_hbg {
namespace {

using parcel::core::ErrorCode;
using parcel::core::MakeError;

template <typename T> bool Finite(const T& value) { return std::isfinite(value); }

bool Finite3(const parcel::Vec3& value) {
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool Finite4(const parcel::Vec4& value) {
    return Finite(value.x) && Finite(value.y) && Finite(value.z) && Finite(value.w);
}

template <typename Target, typename Source> Target ConvertEnum(Source value) {
    return static_cast<Target>(value);
}

} // namespace

Result<hbg::render::PipelineSettings>
SettingsBridge::MapPipeline(const parcel::assets::PipelineSettingsAsset& asset) {
    const parcel::assets::PipelineSettings& source = asset.settings;
    if (source.cookie_atlas_size == 0 || source.cookie_atlas_size > 16384 ||
        source.light_list_capacity == 0 || source.light_list_capacity > 65536) {
        return MakeError(ErrorCode::InvalidArgument, "pipeline settings capacity is invalid");
    }

    hbg::render::PipelineSettings result;
    result.supportedLitShaderMode =
        ConvertEnum<hbg::render::SupportedLitShaderMode>(source.supported_lit_shader_mode);
    result.colorBufferFormat =
        ConvertEnum<hbg::render::ColorBufferFormat>(source.color_buffer_format);
    result.depthBufferFormat =
        ConvertEnum<hbg::render::DepthBufferFormat>(source.depth_buffer_format);
    result.msaaSampleCount = ConvertEnum<hbg::render::MSAASamples>(source.msaa_sample_count);
    result.supportShadows = source.support_shadows;
    result.supportShadowMask = source.support_shadow_mask;
    result.supportGpuLightList = source.support_gpu_light_list;
    result.supportGpuLightListBuffers = source.support_gpu_light_list_buffers;
    result.supportGpuLightListIndirect = source.support_gpu_light_list_indirect;
    result.supportLightListFeatureFlags = source.support_light_list_feature_flags;
    result.supportLitVariants = source.support_lit_variants;
    result.supportLitVariantTiles = source.support_lit_variant_tiles;
    result.supportCookieAtlas = source.support_cookie_atlas;
    result.cookieAtlasSize = source.cookie_atlas_size;
    result.lightListCapacity = source.light_list_capacity;
    result.supportGpuShadowCulling = source.support_gpu_shadow_culling;
    result.supportGpuShadowIndirect = source.support_gpu_shadow_indirect;
    result.supportAsyncGpuLightList = source.support_async_gpu_light_list;
    result.supportContactShadows = source.support_contact_shadows;
    result.supportScreenSpaceShadows = source.support_screen_space_shadows;
    result.supportSSR = source.support_ssr;
    result.supportSSRTransparent = source.support_ssr_transparent;
    result.supportSSAO = source.support_ssao;
    result.supportSSGI = source.support_ssgi;
    result.supportSubsurfaceScattering = source.support_subsurface_scattering;
    result.supportVolumetrics = source.support_volumetrics;
    result.supportVolumetricClouds = source.support_volumetric_clouds;
    result.supportMotionVectors = source.support_motion_vectors;
    result.supportTAA = source.support_taa;
    result.supportBloom = source.support_bloom;
    result.supportColorGrading = source.support_color_grading;
    result.supportDepthOfField = source.support_depth_of_field;
    result.supportMotionBlur = source.support_motion_blur;
    result.supportFXAA = source.support_fxaa;
    result.supportExposureHistogram = source.support_exposure_histogram;
    result.supportTransparentDepthPrepass = source.support_transparent_depth_prepass;
    result.supportTransparentDepthPostpass = source.support_transparent_depth_postpass;
    result.supportLowResTransparent = source.support_low_res_transparent;
    result.supportDecals = source.support_decals;
    result.supportDecalLayers = source.support_decal_layers;
    result.supportDistortion = source.support_distortion;
    result.supportLightLayers = source.support_light_layers;
    result.supportWater = source.support_water;
    result.supportWaterDecals = source.support_water_decals;
    result.supportWaterExclusion = source.support_water_exclusion;
    result.supportRayTracing = source.support_ray_tracing;
    result.supportScreenSpaceLensFlare = source.support_screen_space_lens_flare;
    result.supportDataDrivenLensFlare = source.support_data_driven_lens_flare;
    result.supportCustomPass = source.support_custom_pass;
    result.supportVariableRateShading = source.support_variable_rate_shading;
    result.supportVirtualTexturing = source.support_virtual_texturing;
    result.supportProbeVolume = source.support_probe_volume;
    result.supportHighQualityLineRendering = source.support_high_quality_line_rendering;
    result.supportComputeThickness = source.support_compute_thickness;
    result.supportAsyncCompute = source.support_async_compute;
    result.renderingLayerMaskBuffer = source.rendering_layer_mask_buffer;
    return result;
}

Result<hbg::render::FrameSettings>
SettingsBridge::MapFrame(const parcel::assets::FrameSettingsAsset& asset) {
    const parcel::assets::FrameSettings& source = asset.settings;
    hbg::render::FrameSettings result;
    result.bitDatas = hbg::BitSet128{source.bit_data[0], source.bit_data[1]};
    result.lodBias = source.lod_bias;
    result.lodBiasMode = ConvertEnum<hbg::render::LODBiasMode>(source.lod_bias_mode);
    result.lodBiasQualityLevel = source.lod_bias_quality_level;
    result.maximumLODLevel = source.maximum_lod_level;
    result.maximumLODLevelMode =
        ConvertEnum<hbg::render::MaximumLODLevelMode>(source.maximum_lod_level_mode);
    result.maximumLODLevelQualityLevel = source.maximum_lod_level_quality_level;
    result.sssQualityMode = ConvertEnum<hbg::render::SssQualityMode>(source.sss_quality_mode);
    result.sssQualityLevel = source.sss_quality_level;
    result.sssCustomSampleBudget = source.sss_custom_sample_budget;
    result.sssCustomDownsampleSteps = source.sss_custom_downsample_steps;
    result.msaaMode = ConvertEnum<hbg::render::MSAAMode>(source.msaa_mode);
    result.materialQuality = ConvertEnum<hbg::render::MaterialQuality>(source.material_quality);
    return result;
}

Result<hbg::render::PostProcessProfile>
SettingsBridge::MapPostProcess(const parcel::assets::PostProcessProfileAsset& asset) {
    const parcel::assets::PostProcessProfile& profile = asset.profile;
    const parcel::assets::PostProcessSettings& source = profile.settings;
    if (!Finite(source.limit_min) || !Finite(source.limit_max) ||
        source.limit_max <= source.limit_min || source.histogram_min_percentile < 0.0f ||
        source.histogram_min_percentile > 1.0f || source.histogram_max_percentile < 0.0f ||
        source.histogram_max_percentile > 1.0f || source.bloom.resolution > 8 ||
        source.bloom.resolution == 0 || !Finite3(source.color_grading.color_filter) ||
        !Finite4(source.color_grading.shadows_midtones_highlights_limits)) {
        return MakeError(ErrorCode::InvalidArgument, "post process settings are invalid");
    }

    hbg::render::PostProcessProfile result;
    result.name = profile.name;
    hbg::render::PostProcessSettings& target = result.settings;
    target.exposureMode = ConvertEnum<hbg::render::ExposureMode>(source.exposure_mode);
    target.fixedEv100 = source.fixed_ev100;
    target.compensation = source.compensation;
    target.limitMin = source.limit_min;
    target.limitMax = source.limit_max;
    target.debugExposure = source.debug_exposure;
    target.tonemapping = ConvertEnum<hbg::render::TonemappingMode>(source.tonemapping);
    target.metering = ConvertEnum<hbg::render::MeteringMode>(source.metering);
    target.adaptationSpeedDarkToLight = source.adaptation_speed_dark_to_light;
    target.adaptationSpeedLightToDark = source.adaptation_speed_light_to_dark;
    target.histogramMinPercentile = source.histogram_min_percentile;
    target.histogramMaxPercentile = source.histogram_max_percentile;
    target.proceduralCenter = hbg::Vec3{source.procedural_center.x, source.procedural_center.y,
                                        source.procedural_center.z};
    target.proceduralRadii =
        hbg::Vec3{source.procedural_radii.x, source.procedural_radii.y, source.procedural_radii.z};
    target.proceduralSoftness = source.procedural_softness;

    target.bloom.enabled = source.bloom.enabled;
    target.bloom.threshold = source.bloom.threshold;
    target.bloom.intensity = source.bloom.intensity;
    target.bloom.scatter = source.bloom.scatter;
    target.bloom.tint = hbg::Vec3{source.bloom.tint.x, source.bloom.tint.y, source.bloom.tint.z};
    target.bloom.highQualityPrefiltering = source.bloom.high_quality_prefiltering;
    target.bloom.highQualityFiltering = source.bloom.high_quality_filtering;
    target.bloom.resolution = source.bloom.resolution;

    target.colorGrading.enabled = source.color_grading.enabled;
    target.colorGrading.postExposure = source.color_grading.post_exposure;
    target.colorGrading.contrast = source.color_grading.contrast;
    target.colorGrading.colorFilter =
        hbg::Vec3{source.color_grading.color_filter.x, source.color_grading.color_filter.y,
                  source.color_grading.color_filter.z};
    target.colorGrading.hueShift = source.color_grading.hue_shift;
    target.colorGrading.saturation = source.color_grading.saturation;
    target.colorGrading.temperature = source.color_grading.temperature;
    target.colorGrading.tint = source.color_grading.tint;
    target.colorGrading.channelMixerRed = hbg::Vec3{source.color_grading.channel_mixer_red.x,
                                                    source.color_grading.channel_mixer_red.y,
                                                    source.color_grading.channel_mixer_red.z};
    target.colorGrading.channelMixerGreen = hbg::Vec3{source.color_grading.channel_mixer_green.x,
                                                      source.color_grading.channel_mixer_green.y,
                                                      source.color_grading.channel_mixer_green.z};
    target.colorGrading.channelMixerBlue = hbg::Vec3{source.color_grading.channel_mixer_blue.x,
                                                     source.color_grading.channel_mixer_blue.y,
                                                     source.color_grading.channel_mixer_blue.z};
    target.colorGrading.lift = hbg::Vec3{source.color_grading.lift.x, source.color_grading.lift.y,
                                         source.color_grading.lift.z};
    target.colorGrading.gamma = hbg::Vec3{
        source.color_grading.gamma.x, source.color_grading.gamma.y, source.color_grading.gamma.z};
    target.colorGrading.gain = hbg::Vec3{source.color_grading.gain.x, source.color_grading.gain.y,
                                         source.color_grading.gain.z};
    target.colorGrading.shadows =
        hbg::Vec3{source.color_grading.shadows.x, source.color_grading.shadows.y,
                  source.color_grading.shadows.z};
    target.colorGrading.midtones =
        hbg::Vec3{source.color_grading.midtones.x, source.color_grading.midtones.y,
                  source.color_grading.midtones.z};
    target.colorGrading.highlights =
        hbg::Vec3{source.color_grading.highlights.x, source.color_grading.highlights.y,
                  source.color_grading.highlights.z};
    target.colorGrading.shadowsMidtonesHighlightsLimits =
        hbg::Vec4{source.color_grading.shadows_midtones_highlights_limits.x,
                  source.color_grading.shadows_midtones_highlights_limits.y,
                  source.color_grading.shadows_midtones_highlights_limits.z,
                  source.color_grading.shadows_midtones_highlights_limits.w};
    target.colorGrading.splitShadows =
        hbg::Vec3{source.color_grading.split_shadows.x, source.color_grading.split_shadows.y,
                  source.color_grading.split_shadows.z};
    target.colorGrading.splitHighlights =
        hbg::Vec3{source.color_grading.split_highlights.x, source.color_grading.split_highlights.y,
                  source.color_grading.split_highlights.z};
    target.colorGrading.splitBalance = source.color_grading.split_balance;

    target.vignette.center = hbg::Vec2{source.vignette.center.x, source.vignette.center.y};
    target.vignette.intensity = source.vignette.intensity;
    target.vignette.smoothness = source.vignette.smoothness;
    target.vignette.roundness = source.vignette.roundness;
    target.vignette.rounded = source.vignette.rounded;
    target.vignette.color =
        hbg::Vec3{source.vignette.color.x, source.vignette.color.y, source.vignette.color.z};
    target.vignette.opacity = source.vignette.opacity;

    target.taa.enabled = source.taa.enabled;
    target.taa.quality = source.taa.quality;
    target.taa.motionVectorRejection = source.taa.motion_vector_rejection;
    target.taa.sharpening = source.taa.sharpening;
    target.taa.jitterScale = source.taa.jitter_scale;

    target.depthOfField.enabled = source.depth_of_field.enabled;
    target.depthOfField.focusDistance = source.depth_of_field.focus_distance;
    target.depthOfField.nearFocusStart = source.depth_of_field.near_focus_start;
    target.depthOfField.nearFocusEnd = source.depth_of_field.near_focus_end;
    target.depthOfField.farFocusStart = source.depth_of_field.far_focus_start;
    target.depthOfField.farFocusEnd = source.depth_of_field.far_focus_end;
    target.depthOfField.nearMaxBlur = source.depth_of_field.near_max_blur;
    target.depthOfField.farMaxBlur = source.depth_of_field.far_max_blur;
    target.depthOfField.resolution = source.depth_of_field.resolution;
    target.depthOfField.highQuality = source.depth_of_field.high_quality;

    target.motionBlur.enabled = source.motion_blur.enabled;
    target.motionBlur.intensity = source.motion_blur.intensity;
    target.motionBlur.maximumVelocity = source.motion_blur.maximum_velocity;
    target.motionBlur.minimumVelocity = source.motion_blur.minimum_velocity;
    target.motionBlur.cameraMotionBlur = source.motion_blur.camera_motion_blur;
    target.motionBlur.sampleCount = source.motion_blur.sample_count;
    target.motionBlur.depthComparisonExtent = source.motion_blur.depth_comparison_extent;

    target.fxaa.enabled = source.fxaa.enabled;
    return result;
}

Result<hbg::render::ShadowSettings>
SettingsBridge::MapShadows(const parcel::assets::ShadowSettingsAsset& asset) {
    const parcel::assets::ShadowSettings& source = asset.settings;
    if (source.cascade_count == 0 || source.cascade_count > 4 || source.atlas_size == 0 ||
        source.atlas_size > 16384 || source.area_atlas_size == 0 ||
        source.area_atlas_size > 16384 || source.max_requests == 0 || source.max_requests > 128 ||
        source.directional_resolution == 0 || source.point_resolution == 0 ||
        source.spot_resolution == 0 || source.area_resolution == 0) {
        return MakeError(ErrorCode::InvalidArgument, "shadow settings are invalid");
    }

    hbg::render::ShadowSettings result;
    result.maxDistance = source.max_distance;
    result.cascadeCount = source.cascade_count;
    result.cascadeSplits = source.cascade_splits;
    result.cascadeFade = source.cascade_fade;
    result.directionalFilter = ConvertEnum<hbg::render::ShadowFilter>(source.directional_filter);
    result.punctualFilter = ConvertEnum<hbg::render::ShadowFilter>(source.punctual_filter);
    result.areaFilter = ConvertEnum<hbg::render::ShadowFilter>(source.area_filter);
    result.directionalResolution = source.directional_resolution;
    result.pointResolution = source.point_resolution;
    result.spotResolution = source.spot_resolution;
    result.areaResolution = source.area_resolution;
    result.atlasSize = source.atlas_size;
    result.areaAtlasSize = source.area_atlas_size;
    result.maxRequests = source.max_requests;
    result.normalBias = source.normal_bias;
    result.slopeBias = source.slope_bias;
    result.shadowNearPlane = source.shadow_near_plane;
    result.directionalLightAngle = source.directional_light_angle;
    result.directionalMaxDepthBias = source.directional_max_depth_bias;
    result.imsKernelSize = source.ims_kernel_size;
    result.softness = source.softness;
    result.blockerSamples = source.blocker_samples;
    result.filterSamples = source.filter_samples;
    result.minFilterSize = source.min_filter_size;
    result.microShadows = source.micro_shadows;
    result.microShadowOpacity = source.micro_shadow_opacity;

    result.contactShadows.enabled = source.contact_shadows.enabled;
    result.contactShadows.directionalOnly = source.contact_shadows.directional_only;
    result.contactShadows.halfResolution = source.contact_shadows.half_resolution;
    result.contactShadows.length = source.contact_shadows.length;
    result.contactShadows.opacity = source.contact_shadows.opacity;
    result.contactShadows.distanceScaleFactor = source.contact_shadows.distance_scale_factor;
    result.contactShadows.minDistance = source.contact_shadows.min_distance;
    result.contactShadows.maxDistance = source.contact_shadows.max_distance;
    result.contactShadows.fadeDistance = source.contact_shadows.fade_distance;
    result.contactShadows.fadeInDistance = source.contact_shadows.fade_in_distance;
    result.contactShadows.rayBias = source.contact_shadows.ray_bias;
    result.contactShadows.thicknessScale = source.contact_shadows.thickness_scale;
    result.contactShadows.sampleCount = source.contact_shadows.sample_count;

    result.cachedDirectionalShadows = source.cached_directional_shadows;
    result.cachedPunctualShadows = source.cached_punctual_shadows;
    result.cachedAreaShadows = source.cached_area_shadows;
    result.shadowDimmer = source.shadow_dimmer;
    result.shadowFadeDistance = source.shadow_fade_distance;
    result.shadowTint = hbg::Vec3{source.shadow_tint.x, source.shadow_tint.y, source.shadow_tint.z};
    return result;
}

Result<hbg::render::VolumetricFogSettings>
SettingsBridge::MapVolumetricFog(const parcel::assets::VolumetricFogProfileAsset& asset) {
    const parcel::assets::VolumetricFogSettings& source = asset.settings;
    if (source.step_count == 0 || source.step_count > 256 || source.mean_free_path <= 0.0f ||
        source.maximum_height < source.base_height || !Finite3(source.albedo)) {
        return MakeError(ErrorCode::InvalidArgument, "volumetric fog settings are invalid");
    }

    hbg::render::VolumetricFogSettings result;
    result.enabled = source.enabled;
    result.meanFreePath = source.mean_free_path;
    result.albedo = hbg::Vec3{source.albedo.x, source.albedo.y, source.albedo.z};
    result.baseHeight = source.base_height;
    result.maximumHeight = source.maximum_height;
    result.anisotropy = source.anisotropy;
    result.depthExtent = source.depth_extent;
    result.stepCount = source.step_count;
    result.directionalShadows = source.directional_shadows;
    result.globalLightProbeDimmer = source.global_light_probe_dimmer;
    result.resolutionScale = source.resolution_scale;
    result.temporal = source.temporal;
    result.temporalHistoryWeight = source.temporal_history_weight;
    result.temporalJitter = source.temporal_jitter;
    result.punctualLights = source.punctual_lights;
    result.punctualShadows = source.punctual_shadows;
    result.maxPunctualLights = source.max_punctual_lights;
    result.localVolumes = source.local_volumes;
    return result;
}

} // namespace parcel_hbg
