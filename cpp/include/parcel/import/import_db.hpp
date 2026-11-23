#pragma once

#include "parcel/core/guid.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::import {

inline constexpr std::uint32_t kImportDBVersion = 2;

struct ImportDBOutput {
    core::AssetGuid guid;
    core::Hash256 output_hash;
};

struct ImportDBDependency {
    core::AssetGuid guid;
    core::Hash256 output_hash;
};

struct ImportDBRecord {
    std::string source_path;
    core::AssetGuid source_guid;
    std::string importer;
    std::uint32_t importer_version = 1;
    core::Hash256 source_hash;
    core::Hash256 settings_hash;
    core::Hash256 texture_index_hash;
    std::vector<ImportDBOutput> outputs;
    std::vector<ImportDBDependency> dependencies;
};

struct ImportDB {
    std::vector<ImportDBRecord> records;

    [[nodiscard]] const ImportDBRecord* FindSource(std::string_view source_path) const noexcept;
    [[nodiscard]] const ImportDBOutput* FindOutput(core::AssetGuid guid) const noexcept;
};

[[nodiscard]] core::Result<ImportDB> LoadImportDB(std::string_view path);
[[nodiscard]] core::Result<void> SaveImportDB(std::string_view path, const ImportDB& db);
[[nodiscard]] core::Result<void> SaveImportDB(std::string_view path,
                                              const std::vector<ImportDBRecord>& records);

} // namespace parcel::import
