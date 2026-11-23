#pragma once

#include "parcel/core/guid.hpp"
#include "parcel/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::serialize {

inline constexpr std::uint32_t kBuiltinCatalogMagic = 0x0133C5EEU;
inline constexpr std::int32_t kBuiltinCatalogVersion = 1;
inline constexpr std::size_t kBuiltinCatalogMaxBytes = 100U * 1024U * 1024U;
inline constexpr std::string_view kBuiltinCatalogFileName = "builtin_catalog.bin";

struct BuiltinCatalogEntry {
    core::AssetGuid bundle_guid;
    std::string file_name;
};

struct BuiltinCatalog {
    std::string package_name;
    std::string package_version;
    std::vector<BuiltinCatalogEntry> entries;
};

[[nodiscard]] core::Result<std::vector<std::byte>>
EncodeBuiltinCatalog(const BuiltinCatalog& catalog);

[[nodiscard]] core::Result<BuiltinCatalog> DecodeBuiltinCatalog(std::span<const std::byte> bytes);

} // namespace parcel::serialize
