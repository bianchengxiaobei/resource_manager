#pragma once

#include "parcel/assets/mesh_asset.hpp"
#include "parcel/core/result.hpp"

namespace parcel::import {

struct MeshOptimizeOptions {
    bool optimize_overdraw = false;
    float overdraw_threshold = 1.05F;
};

[[nodiscard]] core::Result<void> OptimizeMesh(assets::MeshAsset& mesh,
                                              const MeshOptimizeOptions& options = {});

} // namespace parcel::import
