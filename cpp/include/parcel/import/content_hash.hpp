#pragma once

#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/mesh_asset.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/result.hpp"

#include <span>

namespace parcel::import {

[[nodiscard]] core::Result<core::Hash256>
HashMaterialContent(const assets::MaterialAsset& material);

[[nodiscard]] core::Result<core::Hash256>
HashMeshContent(const assets::MeshAsset& mesh, std::span<const core::AssetGuid> materials);

} // namespace parcel::import
