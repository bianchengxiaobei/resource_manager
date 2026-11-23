#pragma once

#include "parcel/assets/settings_assets.hpp"
#include "parcel/core/result.hpp"

#include <render/atmosphere/volumetric_fog_settings.h>
#include <render/pipeline/frame_settings.h>
#include <render/pipeline/pipeline_settings.h>
#include <render/postprocess/post_process_profile.h>
#include <render/shadows/shadow_settings.h>

namespace parcel_hbg {

using parcel::core::Result;

class SettingsBridge {
  public:
    [[nodiscard]] static Result<hbg::render::PipelineSettings>
    MapPipeline(const parcel::assets::PipelineSettingsAsset& asset);
    [[nodiscard]] static Result<hbg::render::FrameSettings>
    MapFrame(const parcel::assets::FrameSettingsAsset& asset);
    [[nodiscard]] static Result<hbg::render::PostProcessProfile>
    MapPostProcess(const parcel::assets::PostProcessProfileAsset& asset);
    [[nodiscard]] static Result<hbg::render::ShadowSettings>
    MapShadows(const parcel::assets::ShadowSettingsAsset& asset);
    [[nodiscard]] static Result<hbg::render::VolumetricFogSettings>
    MapVolumetricFog(const parcel::assets::VolumetricFogProfileAsset& asset);
};

} // namespace parcel_hbg
