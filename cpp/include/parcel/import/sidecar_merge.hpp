#pragma once

#include "parcel/assets/sidecar.hpp"
#include "parcel/core/result.hpp"
#include "parcel/import/texture_dedup.hpp"
#include "parcel/scene/scene_data.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace parcel::import {

struct SidecarMergeOptions {
    std::string project_root;
    std::string source_path;
    std::unordered_map<std::string, core::AssetGuid>* texture_aliases = nullptr;
    const TextureCanonicalIndex* canonical_textures = nullptr;
};

[[nodiscard]] core::AssetGuid ResolveSidecarTexture(std::string_view value,
                                                    const SidecarMergeOptions& options,
                                                    std::string_view context,
                                                    std::vector<std::string>& warnings);

[[nodiscard]] core::AssetGuid ResolveSidecarIes(std::string_view value,
                                                const SidecarMergeOptions& options,
                                                std::string_view context,
                                                std::vector<std::string>& warnings);

[[nodiscard]] core::Result<void> MergeSceneSidecar(scene::SceneData& scene,
                                                   const assets::SceneSidecar& sidecar,
                                                   const SidecarMergeOptions& options,
                                                   std::uint64_t& next_entity_id,
                                                   std::vector<std::string>& warnings);

} // namespace parcel::import
