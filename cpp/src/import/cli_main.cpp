#include "parcel/import/importers.hpp"
#include "parcel/import/pipeline.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void PrintUsage() {
    std::cout
        << "Usage: parcel_import --project <root> --source <file-or-dir> --output <dir>\n"
        << "                     [--db <import.db>] [--target <name>] [--jobs <n>]\n"
        << "                     [--shader-manifest <manifest.json>] [--force] [--dry-run]\n"
        << "                     [--shader-build-python <python>] [--shader-build-script <py>]\n"
        << "                     [--shader-build-shaderc <exe>]\n"
        << "                     [--shader-build-bgfx-src <dir>] [--shader-build-root <dir>]\n"
        << "                     [--shader-build-jobs <n>] [--shader-build-timeout <seconds>]\n"
        << "                     [--shader-build-force]\n";
}

bool ParseUInt(std::string_view text, std::uint32_t& value, bool allow_zero = false) {
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc() && result.ptr == text.data() + text.size() &&
           (allow_zero || value > 0);
}

} // namespace

int main(int argc, char** argv) {
    parcel::import::ImportOptions options;
    auto shader_build = [&]() -> parcel::import::ShaderBuildOptions& {
        if (!options.shader_build) {
            options.shader_build.emplace();
        }
        return *options.shader_build;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        auto next = [&](std::string& target) {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << argument << '\n';
                return false;
            }
            target = argv[++i];
            return true;
        };
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return 0;
        }
        if (argument == "--project") {
            if (!next(options.project_root)) {
                return 2;
            }
        } else if (argument == "--source") {
            if (!next(options.source_root)) {
                return 2;
            }
        } else if (argument == "--output") {
            if (!next(options.output_root)) {
                return 2;
            }
        } else if (argument == "--db") {
            if (!next(options.db_path)) {
                return 2;
            }
        } else if (argument == "--target") {
            if (!next(options.target)) {
                return 2;
            }
        } else if (argument == "--shader-manifest") {
            if (!next(options.shader_manifest)) {
                return 2;
            }
        } else if (argument == "--shader-build-python") {
            if (!next(shader_build().python_executable)) {
                return 2;
            }
        } else if (argument == "--shader-build-script") {
            if (!next(shader_build().script)) {
                return 2;
            }
        } else if (argument == "--shader-build-shaderc") {
            if (!next(shader_build().shaderc)) {
                return 2;
            }
        } else if (argument == "--shader-build-bgfx-src") {
            if (!next(shader_build().bgfx_src)) {
                return 2;
            }
        } else if (argument == "--shader-build-root") {
            if (!next(shader_build().root)) {
                return 2;
            }
        } else if (argument == "--shader-build-jobs") {
            std::string value;
            if (!next(value) || !ParseUInt(value, shader_build().jobs)) {
                std::cerr << "invalid --shader-build-jobs value\n";
                return 2;
            }
        } else if (argument == "--shader-build-timeout") {
            std::string value;
            if (!next(value) || !ParseUInt(value, shader_build().timeout_seconds, true)) {
                std::cerr << "invalid --shader-build-timeout value\n";
                return 2;
            }
        } else if (argument == "--shader-build-force") {
            shader_build().force = true;
        } else if (argument == "--jobs") {
            std::string value;
            if (!next(value) || !ParseUInt(value, options.jobs)) {
                std::cerr << "invalid --jobs value\n";
                return 2;
            }
        } else if (argument == "--force") {
            options.force = true;
        } else if (argument == "--dry-run") {
            options.dry_run = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            PrintUsage();
            return 2;
        }
    }

    if (options.project_root.empty() || options.source_root.empty() ||
        options.output_root.empty()) {
        PrintUsage();
        return 2;
    }

    auto importers = parcel::import::CreateStandardImporterRegistry();
    if (!importers) {
        std::cerr << importers.GetError().ToString() << '\n';
        return 1;
    }
    auto summary = parcel::import::RunImport(options, importers.Value());
    if (!summary) {
        std::cerr << summary.GetError().ToString() << '\n';
        return 1;
    }

    for (const auto& entry : summary.Value().entries) {
        std::cout << entry.status << " " << entry.source;
        if (!entry.error.empty()) {
            std::cout << ": " << entry.error;
        }
        std::cout << '\n';
        for (const std::string& warning : entry.warnings) {
            std::cout << "  warning: " << warning << '\n';
        }
    }
    std::cout << "imported=" << summary.Value().imported << " skipped=" << summary.Value().skipped
              << " failed=" << summary.Value().failed << " elapsed=" << summary.Value().elapsed_ms
              << "ms\n";
    return summary.Value().failed == 0 ? 0 : 1;
}
