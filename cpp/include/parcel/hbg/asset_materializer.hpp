#pragma once

#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/mesh_asset.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/result.hpp"

#include <asset/material.h>
#include <asset/mesh.h>
#include <gfx/texture.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace parcel_hbg {

using parcel::core::AssetGuid;
using parcel::core::Result;

struct MaterializedTexture {
    hbg::gfx::TextureDesc desc;
    std::shared_ptr<const std::vector<std::byte>> storage;
};

using ImageIndexResolver = std::function<Result<int>(AssetGuid)>;

[[nodiscard]] Result<MaterializedTexture>
MaterializeTexture(const parcel::assets::TextureAsset& texture, std::string_view name = {});

[[nodiscard]] Result<hbg::asset::Mesh> MaterializeMesh(const parcel::assets::MeshAsset& mesh);

[[nodiscard]] Result<hbg::asset::Material>
MaterializeMaterial(const parcel::assets::MaterialAsset& material,
                    const ImageIndexResolver& resolve_image);

[[nodiscard]] hbg::asset::MaterialKey MakeMaterialKey(const hbg::asset::Material& material);

[[nodiscard]] Result<hbg::asset::MaterialKey>
MakeMaterialKey(const parcel::assets::MaterialAsset& material,
                const ImageIndexResolver& resolve_image);

} // namespace parcel_hbg
