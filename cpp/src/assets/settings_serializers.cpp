#include "parcel/assets/serializers.hpp"
#include "parcel/assets/settings_assets.hpp"
#include "parcel/serialize/math_io.hpp"
#include "serializer_util.hpp"

#include <array>
#include <memory>
#include <string>

namespace parcel::assets::detail {
namespace {

template <std::size_t N>
Result<void> WriteBools(BinaryWriter& writer, const std::array<const bool*, N>& values) {
    for (const bool* value : values) {
        PARCEL_TRY(writer.WriteBool(*value));
    }
    return {};
}

template <std::size_t N>
Result<void> ReadBools(BinaryReader& reader, const std::array<bool*, N>& values) {
    for (bool* value : values) {
        PARCEL_TRY_ASSIGN(*value, reader.ReadBool());
    }
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const PipelineSettings& value) {
    PARCEL_TRY(WriteEnum(writer, value.supported_lit_shader_mode));
    PARCEL_TRY(WriteEnum(writer, value.color_buffer_format));
    PARCEL_TRY(WriteEnum(writer, value.depth_buffer_format));
    PARCEL_TRY(WriteEnum(writer, value.msaa_sample_count));
    const std::array<const bool*, 50> bools = {
        &value.support_shadows,
        &value.support_shadow_mask,
        &value.support_gpu_light_list,
        &value.support_gpu_light_list_buffers,
        &value.support_gpu_light_list_indirect,
        &value.support_light_list_feature_flags,
        &value.support_lit_variants,
        &value.support_lit_variant_tiles,
        &value.support_cookie_atlas,
        &value.support_gpu_shadow_culling,
        &value.support_gpu_shadow_indirect,
        &value.support_async_gpu_light_list,
        &value.support_contact_shadows,
        &value.support_screen_space_shadows,
        &value.support_ssr,
        &value.support_ssr_transparent,
        &value.support_ssao,
        &value.support_ssgi,
        &value.support_subsurface_scattering,
        &value.support_volumetrics,
        &value.support_volumetric_clouds,
        &value.support_motion_vectors,
        &value.support_taa,
        &value.support_bloom,
        &value.support_color_grading,
        &value.support_depth_of_field,
        &value.support_motion_blur,
        &value.support_fxaa,
        &value.support_exposure_histogram,
        &value.support_transparent_depth_prepass,
        &value.support_transparent_depth_postpass,
        &value.support_low_res_transparent,
        &value.support_decals,
        &value.support_decal_layers,
        &value.support_distortion,
        &value.support_light_layers,
        &value.support_water,
        &value.support_water_decals,
        &value.support_water_exclusion,
        &value.support_ray_tracing,
        &value.support_screen_space_lens_flare,
        &value.support_data_driven_lens_flare,
        &value.support_custom_pass,
        &value.support_variable_rate_shading,
        &value.support_virtual_texturing,
        &value.support_probe_volume,
        &value.support_high_quality_line_rendering,
        &value.support_compute_thickness,
        &value.support_async_compute,
        &value.rendering_layer_mask_buffer,
    };
    PARCEL_TRY(WriteBools(writer, bools));
    PARCEL_TRY(writer.WriteU32(value.cookie_atlas_size));
    PARCEL_TRY(writer.WriteU32(value.light_list_capacity));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, PipelineSettings& value) {
    PARCEL_TRY_ASSIGN(value.supported_lit_shader_mode, ReadEnum<SupportedLitShaderMode>(reader, 3));
    PARCEL_TRY_ASSIGN(value.color_buffer_format, ReadEnum<ColorBufferFormat>(reader, 1));
    PARCEL_TRY_ASSIGN(value.depth_buffer_format, ReadEnum<DepthBufferFormat>(reader, 3));
    PARCEL_TRY_ASSIGN(value.msaa_sample_count, ReadEnum<MSAASamples>(reader, 8));
    const std::array<bool*, 50> bools = {
        &value.support_shadows,
        &value.support_shadow_mask,
        &value.support_gpu_light_list,
        &value.support_gpu_light_list_buffers,
        &value.support_gpu_light_list_indirect,
        &value.support_light_list_feature_flags,
        &value.support_lit_variants,
        &value.support_lit_variant_tiles,
        &value.support_cookie_atlas,
        &value.support_gpu_shadow_culling,
        &value.support_gpu_shadow_indirect,
        &value.support_async_gpu_light_list,
        &value.support_contact_shadows,
        &value.support_screen_space_shadows,
        &value.support_ssr,
        &value.support_ssr_transparent,
        &value.support_ssao,
        &value.support_ssgi,
        &value.support_subsurface_scattering,
        &value.support_volumetrics,
        &value.support_volumetric_clouds,
        &value.support_motion_vectors,
        &value.support_taa,
        &value.support_bloom,
        &value.support_color_grading,
        &value.support_depth_of_field,
        &value.support_motion_blur,
        &value.support_fxaa,
        &value.support_exposure_histogram,
        &value.support_transparent_depth_prepass,
        &value.support_transparent_depth_postpass,
        &value.support_low_res_transparent,
        &value.support_decals,
        &value.support_decal_layers,
        &value.support_distortion,
        &value.support_light_layers,
        &value.support_water,
        &value.support_water_decals,
        &value.support_water_exclusion,
        &value.support_ray_tracing,
        &value.support_screen_space_lens_flare,
        &value.support_data_driven_lens_flare,
        &value.support_custom_pass,
        &value.support_variable_rate_shading,
        &value.support_virtual_texturing,
        &value.support_probe_volume,
        &value.support_high_quality_line_rendering,
        &value.support_compute_thickness,
        &value.support_async_compute,
        &value.rendering_layer_mask_buffer,
    };
    PARCEL_TRY(ReadBools(reader, bools));
    PARCEL_TRY_ASSIGN(value.cookie_atlas_size, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.light_list_capacity, reader.ReadU32());
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const FrameSettings& value) {
    PARCEL_TRY(writer.WriteU64(value.bit_data[0]));
    PARCEL_TRY(writer.WriteU64(value.bit_data[1]));
    PARCEL_TRY(writer.WriteF32(value.lod_bias));
    PARCEL_TRY(WriteEnum(writer, value.lod_bias_mode));
    PARCEL_TRY(writer.WriteI32(value.lod_bias_quality_level));
    PARCEL_TRY(writer.WriteI32(value.maximum_lod_level));
    PARCEL_TRY(WriteEnum(writer, value.maximum_lod_level_mode));
    PARCEL_TRY(writer.WriteI32(value.maximum_lod_level_quality_level));
    PARCEL_TRY(WriteEnum(writer, value.sss_quality_mode));
    PARCEL_TRY(writer.WriteI32(value.sss_quality_level));
    PARCEL_TRY(writer.WriteI32(value.sss_custom_sample_budget));
    PARCEL_TRY(writer.WriteI32(value.sss_custom_downsample_steps));
    PARCEL_TRY(WriteEnum(writer, value.msaa_mode));
    PARCEL_TRY(WriteEnum(writer, value.material_quality));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, FrameSettings& value) {
    PARCEL_TRY_ASSIGN(value.bit_data[0], reader.ReadU64());
    PARCEL_TRY_ASSIGN(value.bit_data[1], reader.ReadU64());
    PARCEL_TRY_ASSIGN(value.lod_bias, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.lod_bias_mode, ReadEnum<LODBiasMode>(reader, 2));
    PARCEL_TRY_ASSIGN(value.lod_bias_quality_level, reader.ReadI32());
    PARCEL_TRY_ASSIGN(value.maximum_lod_level, reader.ReadI32());
    PARCEL_TRY_ASSIGN(value.maximum_lod_level_mode, ReadEnum<MaximumLODLevelMode>(reader, 2));
    PARCEL_TRY_ASSIGN(value.maximum_lod_level_quality_level, reader.ReadI32());
    PARCEL_TRY_ASSIGN(value.sss_quality_mode, ReadEnum<SssQualityMode>(reader, 1));
    PARCEL_TRY_ASSIGN(value.sss_quality_level, reader.ReadI32());
    PARCEL_TRY_ASSIGN(value.sss_custom_sample_budget, reader.ReadI32());
    PARCEL_TRY_ASSIGN(value.sss_custom_downsample_steps, reader.ReadI32());
    PARCEL_TRY_ASSIGN(value.msaa_mode, ReadEnum<MSAAMode>(reader, 8));
    PARCEL_TRY_ASSIGN(value.material_quality, ReadEnum<MaterialQuality>(reader, 4));
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const BloomSettings& value) {
    PARCEL_TRY(writer.WriteBool(value.enabled));
    PARCEL_TRY(writer.WriteF32(value.threshold));
    PARCEL_TRY(writer.WriteF32(value.intensity));
    PARCEL_TRY(writer.WriteF32(value.scatter));
    PARCEL_TRY(serialize::WriteVec3(writer, value.tint));
    PARCEL_TRY(writer.WriteBool(value.high_quality_prefiltering));
    PARCEL_TRY(writer.WriteBool(value.high_quality_filtering));
    PARCEL_TRY(writer.WriteU32(value.resolution));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, BloomSettings& value) {
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.threshold, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.intensity, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.scatter, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.tint, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.high_quality_prefiltering, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.high_quality_filtering, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.resolution, reader.ReadU32());
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const ColorGradingSettings& value) {
    PARCEL_TRY(writer.WriteBool(value.enabled));
    PARCEL_TRY(writer.WriteF32(value.post_exposure));
    PARCEL_TRY(writer.WriteF32(value.contrast));
    PARCEL_TRY(serialize::WriteVec3(writer, value.color_filter));
    PARCEL_TRY(writer.WriteF32(value.hue_shift));
    PARCEL_TRY(writer.WriteF32(value.saturation));
    PARCEL_TRY(writer.WriteF32(value.temperature));
    PARCEL_TRY(writer.WriteF32(value.tint));
    PARCEL_TRY(serialize::WriteVec3(writer, value.channel_mixer_red));
    PARCEL_TRY(serialize::WriteVec3(writer, value.channel_mixer_green));
    PARCEL_TRY(serialize::WriteVec3(writer, value.channel_mixer_blue));
    PARCEL_TRY(serialize::WriteVec3(writer, value.lift));
    PARCEL_TRY(serialize::WriteVec3(writer, value.gamma));
    PARCEL_TRY(serialize::WriteVec3(writer, value.gain));
    PARCEL_TRY(serialize::WriteVec3(writer, value.shadows));
    PARCEL_TRY(serialize::WriteVec3(writer, value.midtones));
    PARCEL_TRY(serialize::WriteVec3(writer, value.highlights));
    PARCEL_TRY(serialize::WriteVec4(writer, value.shadows_midtones_highlights_limits));
    PARCEL_TRY(serialize::WriteVec3(writer, value.split_shadows));
    PARCEL_TRY(serialize::WriteVec3(writer, value.split_highlights));
    PARCEL_TRY(writer.WriteF32(value.split_balance));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, ColorGradingSettings& value) {
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.post_exposure, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.contrast, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.color_filter, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.hue_shift, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.saturation, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.temperature, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.tint, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.channel_mixer_red, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.channel_mixer_green, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.channel_mixer_blue, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.lift, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.gamma, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.gain, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.shadows, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.midtones, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.highlights, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.shadows_midtones_highlights_limits, serialize::ReadVec4(reader));
    PARCEL_TRY_ASSIGN(value.split_shadows, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.split_highlights, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.split_balance, reader.ReadF32());
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const VignetteSettings& value) {
    PARCEL_TRY(serialize::WriteVec2(writer, value.center));
    PARCEL_TRY(writer.WriteF32(value.intensity));
    PARCEL_TRY(writer.WriteF32(value.smoothness));
    PARCEL_TRY(writer.WriteF32(value.roundness));
    PARCEL_TRY(writer.WriteBool(value.rounded));
    PARCEL_TRY(serialize::WriteVec3(writer, value.color));
    PARCEL_TRY(writer.WriteF32(value.opacity));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, VignetteSettings& value) {
    PARCEL_TRY_ASSIGN(value.center, serialize::ReadVec2(reader));
    PARCEL_TRY_ASSIGN(value.intensity, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.smoothness, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.roundness, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.rounded, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.color, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.opacity, reader.ReadF32());
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const TaaSettings& value) {
    PARCEL_TRY(writer.WriteBool(value.enabled));
    PARCEL_TRY(writer.WriteU32(value.quality));
    PARCEL_TRY(writer.WriteF32(value.motion_vector_rejection));
    PARCEL_TRY(writer.WriteF32(value.sharpening));
    PARCEL_TRY(writer.WriteF32(value.jitter_scale));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, TaaSettings& value) {
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.quality, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.motion_vector_rejection, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.sharpening, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.jitter_scale, reader.ReadF32());
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const DepthOfFieldSettings& value) {
    PARCEL_TRY(writer.WriteBool(value.enabled));
    PARCEL_TRY(writer.WriteF32(value.focus_distance));
    PARCEL_TRY(writer.WriteF32(value.near_focus_start));
    PARCEL_TRY(writer.WriteF32(value.near_focus_end));
    PARCEL_TRY(writer.WriteF32(value.far_focus_start));
    PARCEL_TRY(writer.WriteF32(value.far_focus_end));
    PARCEL_TRY(writer.WriteF32(value.near_max_blur));
    PARCEL_TRY(writer.WriteF32(value.far_max_blur));
    PARCEL_TRY(writer.WriteU32(value.resolution));
    PARCEL_TRY(writer.WriteBool(value.high_quality));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, DepthOfFieldSettings& value) {
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.focus_distance, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.near_focus_start, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.near_focus_end, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.far_focus_start, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.far_focus_end, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.near_max_blur, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.far_max_blur, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.resolution, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.high_quality, reader.ReadBool());
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const MotionBlurSettings& value) {
    PARCEL_TRY(writer.WriteBool(value.enabled));
    PARCEL_TRY(writer.WriteF32(value.intensity));
    PARCEL_TRY(writer.WriteF32(value.maximum_velocity));
    PARCEL_TRY(writer.WriteF32(value.minimum_velocity));
    PARCEL_TRY(writer.WriteBool(value.camera_motion_blur));
    PARCEL_TRY(writer.WriteU32(value.sample_count));
    PARCEL_TRY(writer.WriteF32(value.depth_comparison_extent));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, MotionBlurSettings& value) {
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.intensity, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.maximum_velocity, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.minimum_velocity, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.camera_motion_blur, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.sample_count, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.depth_comparison_extent, reader.ReadF32());
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const FxaaSettings& value) {
    return writer.WriteBool(value.enabled);
}

Result<void> ReadSettings(BinaryReader& reader, FxaaSettings& value) {
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const PostProcessSettings& value) {
    PARCEL_TRY(WriteEnum(writer, value.exposure_mode));
    PARCEL_TRY(writer.WriteF32(value.fixed_ev100));
    PARCEL_TRY(writer.WriteF32(value.compensation));
    PARCEL_TRY(writer.WriteF32(value.limit_min));
    PARCEL_TRY(writer.WriteF32(value.limit_max));
    PARCEL_TRY(writer.WriteF32(value.debug_exposure));
    PARCEL_TRY(WriteEnum(writer, value.tonemapping));
    PARCEL_TRY(WriteEnum(writer, value.metering));
    PARCEL_TRY(writer.WriteF32(value.adaptation_speed_dark_to_light));
    PARCEL_TRY(writer.WriteF32(value.adaptation_speed_light_to_dark));
    PARCEL_TRY(writer.WriteF32(value.histogram_min_percentile));
    PARCEL_TRY(writer.WriteF32(value.histogram_max_percentile));
    PARCEL_TRY(serialize::WriteVec3(writer, value.procedural_center));
    PARCEL_TRY(serialize::WriteVec3(writer, value.procedural_radii));
    PARCEL_TRY(writer.WriteF32(value.procedural_softness));
    PARCEL_TRY(WriteSettings(writer, value.bloom));
    PARCEL_TRY(WriteSettings(writer, value.color_grading));
    PARCEL_TRY(WriteSettings(writer, value.vignette));
    PARCEL_TRY(WriteSettings(writer, value.taa));
    PARCEL_TRY(WriteSettings(writer, value.depth_of_field));
    PARCEL_TRY(WriteSettings(writer, value.motion_blur));
    PARCEL_TRY(WriteSettings(writer, value.fxaa));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, PostProcessSettings& value) {
    PARCEL_TRY_ASSIGN(value.exposure_mode, ReadEnum<ExposureMode>(reader, 2));
    PARCEL_TRY_ASSIGN(value.fixed_ev100, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.compensation, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.limit_min, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.limit_max, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.debug_exposure, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.tonemapping, ReadEnum<TonemappingMode>(reader, 2));
    PARCEL_TRY_ASSIGN(value.metering, ReadEnum<MeteringMode>(reader, 1));
    PARCEL_TRY_ASSIGN(value.adaptation_speed_dark_to_light, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.adaptation_speed_light_to_dark, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.histogram_min_percentile, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.histogram_max_percentile, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.procedural_center, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.procedural_radii, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.procedural_softness, reader.ReadF32());
    PARCEL_TRY(ReadSettings(reader, value.bloom));
    PARCEL_TRY(ReadSettings(reader, value.color_grading));
    PARCEL_TRY(ReadSettings(reader, value.vignette));
    PARCEL_TRY(ReadSettings(reader, value.taa));
    PARCEL_TRY(ReadSettings(reader, value.depth_of_field));
    PARCEL_TRY(ReadSettings(reader, value.motion_blur));
    PARCEL_TRY(ReadSettings(reader, value.fxaa));
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const PostProcessProfile& value) {
    PARCEL_TRY(writer.WriteString(value.name));
    PARCEL_TRY(WriteSettings(writer, value.settings));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, PostProcessProfile& value) {
    PARCEL_TRY_DECL(std::string_view, name, reader.ReadString());
    value.name.assign(name);
    PARCEL_TRY(ReadSettings(reader, value.settings));
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const ContactShadowSettings& value) {
    PARCEL_TRY(writer.WriteBool(value.enabled));
    PARCEL_TRY(writer.WriteBool(value.directional_only));
    PARCEL_TRY(writer.WriteBool(value.half_resolution));
    PARCEL_TRY(writer.WriteF32(value.length));
    PARCEL_TRY(writer.WriteF32(value.opacity));
    PARCEL_TRY(writer.WriteF32(value.distance_scale_factor));
    PARCEL_TRY(writer.WriteF32(value.min_distance));
    PARCEL_TRY(writer.WriteF32(value.max_distance));
    PARCEL_TRY(writer.WriteF32(value.fade_distance));
    PARCEL_TRY(writer.WriteF32(value.fade_in_distance));
    PARCEL_TRY(writer.WriteF32(value.ray_bias));
    PARCEL_TRY(writer.WriteF32(value.thickness_scale));
    PARCEL_TRY(writer.WriteU32(value.sample_count));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, ContactShadowSettings& value) {
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.directional_only, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.half_resolution, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.length, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.opacity, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.distance_scale_factor, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.min_distance, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.max_distance, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.fade_distance, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.fade_in_distance, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.ray_bias, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.thickness_scale, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.sample_count, reader.ReadU32());
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const ShadowSettings& value) {
    PARCEL_TRY(writer.WriteF32(value.max_distance));
    PARCEL_TRY(writer.WriteU32(value.cascade_count));
    for (float split : value.cascade_splits) {
        PARCEL_TRY(writer.WriteF32(split));
    }
    PARCEL_TRY(writer.WriteF32(value.cascade_fade));
    PARCEL_TRY(WriteEnum(writer, value.directional_filter));
    PARCEL_TRY(WriteEnum(writer, value.punctual_filter));
    PARCEL_TRY(WriteEnum(writer, value.area_filter));
    PARCEL_TRY(writer.WriteU32(value.directional_resolution));
    PARCEL_TRY(writer.WriteU32(value.point_resolution));
    PARCEL_TRY(writer.WriteU32(value.spot_resolution));
    PARCEL_TRY(writer.WriteU32(value.area_resolution));
    PARCEL_TRY(writer.WriteU32(value.atlas_size));
    PARCEL_TRY(writer.WriteU32(value.area_atlas_size));
    PARCEL_TRY(writer.WriteU32(value.max_requests));
    PARCEL_TRY(writer.WriteF32(value.normal_bias));
    PARCEL_TRY(writer.WriteF32(value.slope_bias));
    PARCEL_TRY(writer.WriteF32(value.shadow_near_plane));
    PARCEL_TRY(writer.WriteF32(value.directional_light_angle));
    PARCEL_TRY(writer.WriteF32(value.directional_max_depth_bias));
    PARCEL_TRY(writer.WriteU32(value.ims_kernel_size));
    PARCEL_TRY(writer.WriteF32(value.softness));
    PARCEL_TRY(writer.WriteU32(value.blocker_samples));
    PARCEL_TRY(writer.WriteU32(value.filter_samples));
    PARCEL_TRY(writer.WriteF32(value.min_filter_size));
    PARCEL_TRY(writer.WriteBool(value.micro_shadows));
    PARCEL_TRY(writer.WriteF32(value.micro_shadow_opacity));
    PARCEL_TRY(WriteSettings(writer, value.contact_shadows));
    PARCEL_TRY(writer.WriteBool(value.cached_directional_shadows));
    PARCEL_TRY(writer.WriteBool(value.cached_punctual_shadows));
    PARCEL_TRY(writer.WriteBool(value.cached_area_shadows));
    PARCEL_TRY(writer.WriteF32(value.shadow_dimmer));
    PARCEL_TRY(writer.WriteF32(value.shadow_fade_distance));
    PARCEL_TRY(serialize::WriteVec3(writer, value.shadow_tint));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, ShadowSettings& value) {
    PARCEL_TRY_ASSIGN(value.max_distance, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.cascade_count, reader.ReadU32());
    for (float& split : value.cascade_splits) {
        PARCEL_TRY_ASSIGN(split, reader.ReadF32());
    }
    PARCEL_TRY_ASSIGN(value.cascade_fade, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.directional_filter, ReadEnum<ShadowFilter>(reader, 8));
    PARCEL_TRY_ASSIGN(value.punctual_filter, ReadEnum<ShadowFilter>(reader, 8));
    PARCEL_TRY_ASSIGN(value.area_filter, ReadEnum<ShadowFilter>(reader, 8));
    PARCEL_TRY_ASSIGN(value.directional_resolution, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.point_resolution, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.spot_resolution, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.area_resolution, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.atlas_size, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.area_atlas_size, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.max_requests, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.normal_bias, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.slope_bias, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.shadow_near_plane, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.directional_light_angle, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.directional_max_depth_bias, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.ims_kernel_size, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.softness, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.blocker_samples, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.filter_samples, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.min_filter_size, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.micro_shadows, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.micro_shadow_opacity, reader.ReadF32());
    PARCEL_TRY(ReadSettings(reader, value.contact_shadows));
    PARCEL_TRY_ASSIGN(value.cached_directional_shadows, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.cached_punctual_shadows, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.cached_area_shadows, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.shadow_dimmer, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.shadow_fade_distance, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.shadow_tint, serialize::ReadVec3(reader));
    return {};
}

Result<void> WriteSettings(BinaryWriter& writer, const VolumetricFogSettings& value) {
    PARCEL_TRY(writer.WriteBool(value.enabled));
    PARCEL_TRY(writer.WriteF32(value.mean_free_path));
    PARCEL_TRY(serialize::WriteVec3(writer, value.albedo));
    PARCEL_TRY(writer.WriteF32(value.base_height));
    PARCEL_TRY(writer.WriteF32(value.maximum_height));
    PARCEL_TRY(writer.WriteF32(value.anisotropy));
    PARCEL_TRY(writer.WriteF32(value.depth_extent));
    PARCEL_TRY(writer.WriteU32(value.step_count));
    PARCEL_TRY(writer.WriteBool(value.directional_shadows));
    PARCEL_TRY(writer.WriteF32(value.global_light_probe_dimmer));
    PARCEL_TRY(writer.WriteF32(value.resolution_scale));
    PARCEL_TRY(writer.WriteBool(value.temporal));
    PARCEL_TRY(writer.WriteF32(value.temporal_history_weight));
    PARCEL_TRY(writer.WriteBool(value.temporal_jitter));
    PARCEL_TRY(writer.WriteBool(value.punctual_lights));
    PARCEL_TRY(writer.WriteBool(value.punctual_shadows));
    PARCEL_TRY(writer.WriteU32(value.max_punctual_lights));
    PARCEL_TRY(writer.WriteBool(value.local_volumes));
    return {};
}

Result<void> ReadSettings(BinaryReader& reader, VolumetricFogSettings& value) {
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.mean_free_path, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.albedo, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.base_height, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.maximum_height, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.anisotropy, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.depth_extent, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.step_count, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.directional_shadows, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.global_light_probe_dimmer, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.resolution_scale, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.temporal, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.temporal_history_weight, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.temporal_jitter, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.punctual_lights, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.punctual_shadows, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.max_punctual_lights, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.local_volumes, reader.ReadBool());
    return {};
}

template <typename TAsset, typename TSettings>
class SettingsSerializer final : public ITypeSerializer {
  public:
    SettingsSerializer(TypeId type, std::string_view name) : type_(type), name_(name) {}

    TypeId Type() const noexcept override { return type_; }
    std::string_view TypeName() const noexcept override { return name_; }
    std::uint32_t Version() const noexcept override { return 1; }

    Result<std::shared_ptr<AssetObject>> Create() const override {
        return std::static_pointer_cast<AssetObject>(std::make_shared<TAsset>());
    }

    Result<void> Serialize(const AssetObject& object, BinaryWriter& writer,
                           IAssetWriteContext&) const override {
        if (object.Type() != type_) {
            return MakeError(ErrorCode::InvalidArgument, "settings asset type mismatch");
        }
        return WriteSettings(writer, static_cast<const TAsset&>(object).settings);
    }

    Result<std::shared_ptr<AssetObject>> Deserialize(BinaryReader& reader,
                                                     IAssetReadContext&) const override {
        auto asset = std::make_shared<TAsset>();
        PARCEL_TRY(ReadSettings(reader, asset->settings));
        PARCEL_TRY(Finish(reader, "settings metadata"));
        return std::static_pointer_cast<AssetObject>(asset);
    }

    Result<void> CollectReferences(const AssetObject&, std::vector<AssetGuid>&) const override {
        return {};
    }

  private:
    TypeId type_;
    std::string_view name_;
};

template <typename TAsset> class ProfileSerializer final : public ITypeSerializer {
  public:
    ProfileSerializer(TypeId type, std::string_view name) : type_(type), name_(name) {}

    TypeId Type() const noexcept override { return type_; }
    std::string_view TypeName() const noexcept override { return name_; }
    std::uint32_t Version() const noexcept override { return 1; }

    Result<std::shared_ptr<AssetObject>> Create() const override {
        return std::static_pointer_cast<AssetObject>(std::make_shared<TAsset>());
    }

    Result<void> Serialize(const AssetObject& object, BinaryWriter& writer,
                           IAssetWriteContext&) const override {
        if (object.Type() != type_) {
            return MakeError(ErrorCode::InvalidArgument, "settings asset type mismatch");
        }
        return WriteSettings(writer, static_cast<const TAsset&>(object).profile);
    }

    Result<std::shared_ptr<AssetObject>> Deserialize(BinaryReader& reader,
                                                     IAssetReadContext&) const override {
        auto asset = std::make_shared<TAsset>();
        PARCEL_TRY(ReadSettings(reader, asset->profile));
        PARCEL_TRY(Finish(reader, "settings metadata"));
        return std::static_pointer_cast<AssetObject>(asset);
    }

    Result<void> CollectReferences(const AssetObject&, std::vector<AssetGuid>&) const override {
        return {};
    }

  private:
    TypeId type_;
    std::string_view name_;
};

} // namespace

} // namespace parcel::assets::detail

namespace parcel::assets {

bool FrameSettings::IsEnabled(FrameSettingsField field) const noexcept {
    const auto index = static_cast<std::int16_t>(field);
    if (index < 0 || index >= 128) {
        return false;
    }
    const std::size_t word = static_cast<std::size_t>(index) / 64U;
    const std::size_t bit = static_cast<std::size_t>(index) % 64U;
    return (bit_data[word] & (std::uint64_t{1} << bit)) != 0;
}

void FrameSettings::SetEnabled(FrameSettingsField field, bool value) noexcept {
    const auto index = static_cast<std::int16_t>(field);
    if (index < 0 || index >= 128) {
        return;
    }
    const std::size_t word = static_cast<std::size_t>(index) / 64U;
    const std::size_t bit = static_cast<std::size_t>(index) % 64U;
    if (value) {
        bit_data[word] |= std::uint64_t{1} << bit;
    } else {
        bit_data[word] &= ~(std::uint64_t{1} << bit);
    }
}

} // namespace parcel::assets

namespace parcel::assets::detail {

std::shared_ptr<const ITypeSerializer> MakePipelineSettingsAssetSerializer() {
    return std::make_shared<SettingsSerializer<PipelineSettingsAsset, PipelineSettings>>(
        kPipelineSettingsAssetType, "PipelineSettingsAsset");
}

std::shared_ptr<const ITypeSerializer> MakeFrameSettingsAssetSerializer() {
    return std::make_shared<SettingsSerializer<FrameSettingsAsset, FrameSettings>>(
        kFrameSettingsAssetType, "FrameSettingsAsset");
}

std::shared_ptr<const ITypeSerializer> MakePostProcessProfileAssetSerializer() {
    return std::make_shared<ProfileSerializer<PostProcessProfileAsset>>(
        kPostProcessProfileAssetType, "PostProcessProfileAsset");
}

std::shared_ptr<const ITypeSerializer> MakeShadowSettingsAssetSerializer() {
    return std::make_shared<SettingsSerializer<ShadowSettingsAsset, ShadowSettings>>(
        kShadowSettingsAssetType, "ShadowSettingsAsset");
}

std::shared_ptr<const ITypeSerializer> MakeVolumetricFogProfileAssetSerializer() {
    return std::make_shared<SettingsSerializer<VolumetricFogProfileAsset, VolumetricFogSettings>>(
        kVolumetricFogProfileAssetType, "VolumetricFogProfileAsset");
}

} // namespace parcel::assets::detail
