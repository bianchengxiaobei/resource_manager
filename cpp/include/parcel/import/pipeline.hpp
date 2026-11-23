#pragma once

#include "parcel/import/importer.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace parcel::import {

struct ShaderBuildOptions {
    std::string python_executable;
    std::string script;
    std::string shaderc;
    std::string bgfx_src;
    std::string root;
    std::uint32_t jobs = 0;
    std::uint32_t timeout_seconds = 1800;
    bool force = false;
};

struct ImportOptions {
    std::string project_root;
    std::string source_root;
    std::string output_root;
    std::string db_path;
    std::string target = "windows";
    std::string shader_manifest;
    std::optional<ShaderBuildOptions> shader_build;
    std::uint32_t jobs = 1;
    bool force = false;
    bool dry_run = false;
};

struct ImportReportEntry {
    std::string source;
    std::string importer;
    std::uint32_t importer_version = 0;
    std::string settings_hash;
    std::string status;
    std::string error;
    std::vector<std::string> assets;
    std::vector<std::string> warnings;
};

struct ImportSummary {
    std::uint32_t imported = 0;
    std::uint32_t skipped = 0;
    std::uint32_t failed = 0;
    std::uint64_t elapsed_ms = 0;
    std::vector<ImportReportEntry> entries;
};

[[nodiscard]] core::Result<std::vector<std::string>>
DiscoverImportSources(const ImportOptions& options, std::span<const std::string> explicit_sources,
                      const ImporterRegistry& importers);

[[nodiscard]] core::Result<ImportSummary> RunImport(const ImportOptions& options,
                                                    std::span<const std::string> explicit_sources,
                                                    const ImporterRegistry& importers);

[[nodiscard]] core::Result<ImportSummary> RunImport(const ImportOptions& options,
                                                    const ImporterRegistry& importers);

} // namespace parcel::import
