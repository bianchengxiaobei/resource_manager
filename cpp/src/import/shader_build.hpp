#pragma once

#include "parcel/core/result.hpp"
#include "parcel/import/pipeline.hpp"

#include <string>
#include <string_view>

namespace parcel::import::detail {

struct ShaderBuildResult {
    std::string manifest_path;
};

[[nodiscard]] core::Result<ShaderBuildResult>
RunShaderBuild(const ShaderBuildOptions& options, std::string_view project_root,
               std::string_view expected_manifest = {});

} // namespace parcel::import::detail
