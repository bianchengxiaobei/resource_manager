#pragma once

#include "parcel/core/guid.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/result.hpp"
#include "parcel/serialize/compression.hpp"
#include "parcel/serialize/format.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace parcel::pack {

using core::Result;

enum class CollectorType : std::uint8_t {
    Main,
    Static,
    Depend,
};

enum class AddressRule : std::uint8_t {
    AssetPath,
    FileName,
    Guid,
    Prefix,
    Address,
};

enum class PackRule : std::uint8_t {
    Default,
    PackSeparately,
    PackDirectory,
    PackTopDirectory,
    PackCollector,
    PackGroup,
    PackRawFile,
    PackVideoFile,
    PackShader,
    PackShaderVariants,
};

enum class OutputNameStyle : std::uint8_t {
    HashName = 1,
    BundleName = 2,
    BundleNameHashName = 3,
};

enum class BuildBundleType : std::uint8_t {
    AssetBundle = 1,
    RawFile = 2,
    Archive = 3,
};

enum class FilterRule : std::uint8_t {
    CollectAll,
    CollectScene,
    CollectShader,
    CollectRaw,
    CollectVideo,
    CollectTexture,
    CollectMaterial,
    CollectMesh,
    CollectAnimation,
    CollectText,
    CollectIES,
    CollectPrefab,
    CollectSprite,
    CollectShaderVariants,
};

struct CollectorConfig {
    std::string name;
    CollectorType type = CollectorType::Main;
    std::string path;
    std::vector<std::string> files;
    std::vector<std::string> include_patterns;
    std::vector<std::string> exclude_patterns;
    bool recursive = true;
    AddressRule address_rule = AddressRule::AssetPath;
    std::string address_prefix;
    std::string address;
    std::vector<std::string> tags;
    std::string bundle;
    std::string group;
    PackRule pack_rule = PackRule::Default;
    FilterRule filter_rule = FilterRule::CollectAll;
    bool share = true;
    bool archive = false;
};

struct PackConfig {
    std::string project_root;
    std::string package_name;
    std::string package_version;
    std::string target = "windows";
    std::string source_root;
    std::string cooked_root;
    std::string output_root;

    serialize::CompressionCodec compression = serialize::CompressionCodec::Lz4;
    std::uint32_t block_size = serialize::kDefaultBlockSize;
    int compression_level = 3;
    std::uint32_t jobs = 1;

    bool enable_addressable = true;
    bool support_extensionless = false;
    bool location_to_lower = false;
    bool include_asset_guid = false;
    bool replace_asset_path_with_address = false;
    bool use_source_path = true;
    bool force_sub_asset_grouping = true;
    bool auto_collect_shaders = true;
    bool enable_share_bundle = true;
    bool single_referenced_pack_alone = false;
    bool write_builtin_catalog = true;
    bool clean_output = false;
    bool verify_output_hashes = false;

    std::uint32_t output_name_style = 1;
    std::uint32_t build_bundle_type = 1;

    std::vector<CollectorConfig> collectors;
};

struct PackAssetReport {
    core::AssetGuid guid;
    std::string path;
    std::string source_path;
    std::string type;
    std::string address;
    std::string bundle;
    std::uint64_t size = 0;
    std::uint32_t dependency_count = 0;
    bool main = false;
};

struct PackBundleReport {
    core::AssetGuid guid;
    std::string name;
    std::string file_name;
    std::uint64_t size = 0;
    std::uint32_t asset_count = 0;
    bool reused = false;
    std::vector<std::string> dependencies;
};

struct PackReport {
    std::string tool_version = "0.4.0";
    std::string package_name;
    std::string package_version;
    std::string target;
    std::string compression;
    std::uint32_t block_size = 0;
    int compression_level = 3;
    bool enable_addressable = true;
    bool enable_share_bundle = true;
    bool single_referenced_pack_alone = false;
    bool auto_collect_shaders = true;
    bool output_verified = false;

    std::uint64_t total_asset_bytes = 0;
    std::uint64_t total_bundle_bytes = 0;
    std::uint64_t manifest_bytes = 0;
    std::uint64_t builtin_catalog_bytes = 0;
    std::uint32_t asset_count = 0;
    std::uint32_t main_asset_count = 0;
    std::uint32_t bundle_count = 0;
    std::uint32_t reused_bundle_count = 0;
    std::uint32_t reused_asset_count = 0;
    std::uint32_t verified_bundle_count = 0;
    std::uint32_t shader_variants = 0;
    std::uint32_t shader_backends = 0;

    std::vector<std::pair<std::string, std::uint32_t>> asset_types;
    std::vector<std::pair<std::string, std::uint32_t>> texture_formats;
    std::vector<PackAssetReport> assets;
    std::vector<PackBundleReport> bundles;
    std::vector<std::string> unused_assets;
    std::vector<std::string> zero_reference_assets;
    std::vector<std::string> duplicate_assets;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

struct PackSummary {
    std::string output_directory;
    PackReport report;
};

struct PackVerifyResult {
    std::uint32_t bundle_count = 0;
    bool catalog_verified = false;
};

[[nodiscard]] Result<PackConfig> LoadPackConfig(std::string_view path);

[[nodiscard]] Result<PackSummary> RunPack(const PackConfig& config);

[[nodiscard]] Result<PackVerifyResult> VerifyPackOutput(std::string_view output_directory);

} // namespace parcel::pack
