#pragma once

#include "parcel/import/importer.hpp"
#include "parcel/import/texture_dedup.hpp"

#include <span>
#include <string>
#include <string_view>

namespace parcel::import::detail {

[[nodiscard]] core::Result<TextureCanonicalIndex>
BuildTextureCanonicalIndex(std::string_view project_root, std::string_view target,
                           std::span<const std::string> sources, const ImporterRegistry& importers);

} // namespace parcel::import::detail
