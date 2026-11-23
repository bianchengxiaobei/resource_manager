#pragma once

#include "parcel/assets/shader_library_asset.hpp"
#include "parcel/core/error.hpp"
#include "parcel/core/result.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace parcel_hbg {

using parcel::core::Result;

struct ShaderMaterializeOptions {
    std::filesystem::path cache_root;
    std::string package;
};

struct ShaderMaterialization {
    std::filesystem::path root;
    std::string content_hash;
    std::vector<std::filesystem::path> files;
};

class ShaderLibraryBootstrap {
  public:
    [[nodiscard]] static Result<std::string>
    ContentHash(const parcel::assets::ShaderLibraryAsset& asset);
    [[nodiscard]] static Result<ShaderMaterialization>
    Materialize(const parcel::assets::ShaderLibraryAsset& asset,
                const ShaderMaterializeOptions& options);
    [[nodiscard]] static Result<std::size_t>
    CleanupOldVersions(const ShaderMaterializeOptions& options, std::string_view keep_hash);

    template <typename ShaderLibraryT, typename ResourceManagerT>
    [[nodiscard]] static Result<void> Initialize(ShaderLibraryT& library,
                                                 ResourceManagerT& resources,
                                                 const ShaderMaterialization& materialized) {
        if (!library.init(resources, materialized.root)) {
            return parcel::core::MakeError(parcel::core::ErrorCode::Internal,
                                           "ShaderLibrary::init failed");
        }
        return {};
    }
};

} // namespace parcel_hbg
