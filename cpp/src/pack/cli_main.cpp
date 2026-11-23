#include "parcel/pack/pack.hpp"

#include "parcel/core/error.hpp"
#include "parcel/serialize/compression.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

using parcel::core::ErrorCode;
using parcel::core::MakeError;
using parcel::core::Result;

void PrintHelp() {
    std::cout << "Parcel packer\n\n"
              << "Usage:\n"
              << "  parcel_pack --project <dir> --cooked <dir> --output <dir> "
                 "--package <name> --version <version> [options]\n\n"
              << "Options:\n"
              << "  --config <file>       JSON collector/build configuration\n"
              << "  --target <name>       Target platform (default: windows)\n"
              << "  --jobs <count>        Worker count hint (default: 1)\n"
              << "  --compression <name>  none, lz4 or zstd\n"
              << "  --block-size <bytes>  PRCL block size\n"
              << "  --clean               Remove stale bundle files\n"
              << "  --help                Show this help\n";
}

bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

Result<std::uint32_t> ParseU32(std::string_view value, std::string_view what) {
    if (value.empty()) {
        return MakeError(ErrorCode::InvalidArgument, std::string(what) + " is empty");
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(std::string(value).c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return MakeError(ErrorCode::InvalidArgument, std::string(what) + " is invalid");
    }
    return static_cast<std::uint32_t>(parsed);
}

Result<parcel::serialize::CompressionCodec> ParseCompression(std::string_view value) {
    if (value == "none") {
        return parcel::serialize::CompressionCodec::None;
    }
    if (value == "lz4") {
        return parcel::serialize::CompressionCodec::Lz4;
    }
    if (value == "zstd") {
        return parcel::serialize::CompressionCodec::Zstd;
    }
    return MakeError(ErrorCode::InvalidArgument,
                     "unknown compression codec '" + std::string(value) + "'");
}

} // namespace

int main(int argc, char** argv) {
    parcel::pack::PackConfig config;
    std::string config_path;
    bool help = false;
    bool target_set = false;
    bool compression_set = false;
    bool block_size_set = false;
    bool jobs_set = false;

    auto next_value = [&](int& index, std::string_view option) -> std::string {
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << option << "\n";
            return {};
        }
        ++index;
        return argv[index];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            help = true;
            continue;
        }
        if (StartsWith(arg, "--project=")) {
            config.project_root = std::string(arg.substr(10));
        } else if (arg == "--project") {
            config.project_root = next_value(i, arg);
        } else if (StartsWith(arg, "--package=")) {
            config.package_name = std::string(arg.substr(10));
        } else if (arg == "--package") {
            config.package_name = next_value(i, arg);
        } else if (StartsWith(arg, "--version=")) {
            config.package_version = std::string(arg.substr(10));
        } else if (arg == "--version") {
            config.package_version = next_value(i, arg);
        } else if (StartsWith(arg, "--target=")) {
            config.target = std::string(arg.substr(9));
            target_set = true;
        } else if (arg == "--target") {
            config.target = next_value(i, arg);
            target_set = true;
        } else if (StartsWith(arg, "--cooked=")) {
            config.cooked_root = std::string(arg.substr(9));
        } else if (arg == "--cooked") {
            config.cooked_root = next_value(i, arg);
        } else if (StartsWith(arg, "--output=")) {
            config.output_root = std::string(arg.substr(9));
        } else if (arg == "--output") {
            config.output_root = next_value(i, arg);
        } else if (StartsWith(arg, "--config=")) {
            config_path = std::string(arg.substr(9));
        } else if (arg == "--config") {
            config_path = next_value(i, arg);
        } else if (StartsWith(arg, "--jobs=")) {
            auto parsed = ParseU32(arg.substr(7), "jobs");
            if (!parsed) {
                std::cerr << parsed.GetError().ToString() << "\n";
                return 1;
            }
            config.jobs = parsed.Value();
            jobs_set = true;
        } else if (arg == "--jobs") {
            auto parsed = ParseU32(next_value(i, arg), "jobs");
            if (!parsed) {
                std::cerr << parsed.GetError().ToString() << "\n";
                return 1;
            }
            config.jobs = parsed.Value();
            jobs_set = true;
        } else if (StartsWith(arg, "--compression=")) {
            auto parsed = ParseCompression(arg.substr(14));
            if (!parsed) {
                std::cerr << parsed.GetError().ToString() << "\n";
                return 1;
            }
            config.compression = parsed.Value();
            compression_set = true;
        } else if (arg == "--compression") {
            auto parsed = ParseCompression(next_value(i, arg));
            if (!parsed) {
                std::cerr << parsed.GetError().ToString() << "\n";
                return 1;
            }
            config.compression = parsed.Value();
            compression_set = true;
        } else if (StartsWith(arg, "--block-size=")) {
            auto parsed = ParseU32(arg.substr(13), "block size");
            if (!parsed) {
                std::cerr << parsed.GetError().ToString() << "\n";
                return 1;
            }
            config.block_size = parsed.Value();
            block_size_set = true;
        } else if (arg == "--block-size") {
            auto parsed = ParseU32(next_value(i, arg), "block size");
            if (!parsed) {
                std::cerr << parsed.GetError().ToString() << "\n";
                return 1;
            }
            config.block_size = parsed.Value();
            block_size_set = true;
        } else if (arg == "--clean") {
            config.clean_output = true;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            PrintHelp();
            return 1;
        }
    }

    if (help) {
        PrintHelp();
        return 0;
    }

    if (!config_path.empty()) {
        auto loaded = parcel::pack::LoadPackConfig(config_path);
        if (!loaded) {
            std::cerr << "failed to load pack config: " << loaded.GetError().ToString() << "\n";
            return 1;
        }
        parcel::pack::PackConfig from_file = std::move(loaded).Value();
        if (config.project_root.empty()) {
            config.project_root = std::move(from_file.project_root);
        }
        if (config.package_name.empty()) {
            config.package_name = std::move(from_file.package_name);
        }
        if (config.package_version.empty()) {
            config.package_version = std::move(from_file.package_version);
        }
        if (config.target == "windows" && !target_set) {
            config.target = std::move(from_file.target);
        }
        if (config.cooked_root.empty()) {
            config.cooked_root = std::move(from_file.cooked_root);
        }
        if (config.output_root.empty()) {
            config.output_root = std::move(from_file.output_root);
        }
        if (!jobs_set) {
            config.jobs = from_file.jobs;
        }
        if (!compression_set) {
            config.compression = from_file.compression;
        }
        if (!block_size_set) {
            config.block_size = from_file.block_size;
        }
        config.compression_level = from_file.compression_level;
        config.enable_addressable = from_file.enable_addressable;
        config.support_extensionless = from_file.support_extensionless;
        config.location_to_lower = from_file.location_to_lower;
        config.include_asset_guid = from_file.include_asset_guid;
        config.replace_asset_path_with_address = from_file.replace_asset_path_with_address;
        config.auto_collect_shaders = from_file.auto_collect_shaders;
        config.enable_share_bundle = from_file.enable_share_bundle;
        config.single_referenced_pack_alone = from_file.single_referenced_pack_alone;
        config.clean_output = config.clean_output || from_file.clean_output;
        config.output_name_style = from_file.output_name_style;
        config.build_bundle_type = from_file.build_bundle_type;
        config.collectors = std::move(from_file.collectors);
    }

    if (config.project_root.empty()) {
        config.project_root = ".";
    }
    if (config.cooked_root.empty() || config.output_root.empty() || config.package_name.empty() ||
        config.package_version.empty()) {
        std::cerr << "package, version, cooked and output are required\n";
        PrintHelp();
        return 1;
    }

    auto summary = parcel::pack::RunPack(config);
    if (!summary) {
        std::cerr << "pack failed: " << summary.GetError().ToString() << "\n";
        return 1;
    }
    const parcel::pack::PackReport& report = summary.Value().report;
    std::cout << "packed " << report.asset_count << " assets into " << report.bundle_count
              << " bundles (" << report.reused_bundle_count << " reused), "
              << report.total_bundle_bytes << " bytes\n";
    std::cout << "output: " << summary.Value().output_directory << "\n";
    std::cout << "report: " << summary.Value().output_directory << "/build_report.json\n";
    return 0;
}
