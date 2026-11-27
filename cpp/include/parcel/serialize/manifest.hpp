#pragma once

#include "parcel/core/guid.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/result.hpp"
#include "parcel/serialize/format.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace parcel::serialize {

using core::Result;

inline constexpr std::uint32_t kManifestFlagEnableAddressable = 1U << 0U;
inline constexpr std::uint32_t kManifestFlagSupportExtensionless = 1U << 1U;
inline constexpr std::uint32_t kManifestFlagLocationToLower = 1U << 2U;
inline constexpr std::uint32_t kManifestFlagIncludeAssetGuid = 1U << 3U;
inline constexpr std::uint32_t kManifestFlagReplaceAssetPathWithAddress = 1U << 4U;
inline constexpr std::uint32_t kManifestAssetFlagArchiveRaw = 1U << 0U;
inline constexpr std::size_t kManifestContentHashOffset = 96;

struct ManifestHeader {
    std::uint16_t version = kManifestVersion;
    std::uint16_t header_size = kManifestHeaderSize;
    std::uint32_t flags = 0;
    std::uint64_t file_size = 0;
    std::uint64_t string_table_offset = 0;
    std::uint32_t string_count = 0;
    std::uint64_t asset_table_offset = 0;
    std::uint32_t asset_count = 0;
    std::uint64_t bundle_table_offset = 0;
    std::uint32_t bundle_count = 0;
    std::uint64_t tag_table_offset = 0;
    std::uint32_t tag_count = 0;
    std::uint64_t dependency_table_offset = 0;
    std::uint32_t dependency_count = 0;
    std::uint32_t package_name_index = 0;
    std::uint32_t package_version_index = 0;
    std::uint32_t output_name_style = 0;
    std::uint32_t build_bundle_type = 0;
    core::Hash256 content_hash;
};

struct ManifestAssetDesc {
    core::AssetGuid asset_guid;
    std::string address;
    std::string asset_path;
    std::string type_name;
    std::uint32_t bundle_id = 0;
    std::uint32_t payload_id = 0;
    std::uint32_t local_object_id = 0;
    std::vector<std::uint32_t> dependent_bundle_ids;
    std::vector<std::string> tags;
    std::uint32_t flags = 0;
};

struct ManifestBundleDesc {
    core::AssetGuid bundle_guid;
    std::string bundle_name;
    std::string file_name;
    std::uint64_t file_size = 0;
    core::Hash256 file_sha256;
    std::uint32_t file_crc = 0;
    std::uint32_t flags = 0;
    std::vector<std::uint32_t> dependent_bundle_ids;
    std::vector<std::uint32_t> referrer_bundle_ids;
    std::vector<std::string> tags;
};

struct ManifestDesc {
    std::string package_name;
    std::string package_version;
    std::uint32_t flags = 0;
    std::uint32_t output_name_style = 0;
    std::uint32_t build_bundle_type = 0;
    std::vector<std::string> tags;
    std::vector<ManifestAssetDesc> assets;
    std::vector<ManifestBundleDesc> bundles;
};

struct ManifestAssetView {
    core::AssetGuid asset_guid;
    std::string_view address;
    std::string_view asset_path;
    std::string_view type_name;
    std::uint32_t bundle_index = 0;
    std::uint32_t payload_id = 0;
    std::uint32_t local_object_id = 0;
    std::uint32_t flags = 0;
    std::vector<std::uint32_t> dependent_bundle_ids;
    std::vector<std::string_view> tags;
};

struct ManifestBundleView {
    core::AssetGuid bundle_guid;
    std::string_view bundle_name;
    std::string_view file_name;
    std::uint64_t file_size = 0;
    core::Hash256 file_sha256;
    std::uint32_t file_crc = 0;
    std::uint32_t flags = 0;
    std::vector<std::uint32_t> dependent_bundle_ids;
    std::vector<std::uint32_t> referrer_bundle_ids;
    std::vector<std::string_view> tags;
};

[[nodiscard]] Result<std::vector<std::byte>> WriteManifest(const ManifestDesc& desc);

class ManifestWriter {
  public:
    explicit ManifestWriter(ManifestDesc desc) : desc_(std::move(desc)) {}
    [[nodiscard]] Result<std::vector<std::byte>> Build() const;

  private:
    ManifestDesc desc_;
};

class ManifestView {
  public:
    static Result<ManifestView> Open(std::span<const std::byte> data,
                                     bool verify_content_hash = true);
    static Result<ManifestView> Open(std::shared_ptr<const std::vector<std::byte>> storage,
                                     bool verify_content_hash = true);

    [[nodiscard]] const ManifestHeader& Header() const noexcept { return header_; }
    [[nodiscard]] std::span<const ManifestAssetView> Assets() const noexcept { return assets_; }
    [[nodiscard]] std::span<const ManifestBundleView> Bundles() const noexcept { return bundles_; }
    [[nodiscard]] std::span<const std::string_view> Tags() const noexcept { return tags_; }
    [[nodiscard]] std::string_view PackageName() const noexcept { return package_name_; }
    [[nodiscard]] std::string_view PackageVersion() const noexcept { return package_version_; }

    [[nodiscard]] const ManifestAssetView*
    FindAssetByGuid(const core::AssetGuid& guid) const noexcept;
    [[nodiscard]] const ManifestAssetView*
    FindAssetByAddress(std::string_view address) const noexcept;
    [[nodiscard]] const ManifestAssetView* FindAssetByPath(std::string_view path) const noexcept;
    [[nodiscard]] const ManifestBundleView*
    FindBundleByGuid(const core::AssetGuid& guid) const noexcept;
    [[nodiscard]] const ManifestBundleView* FindBundleByName(std::string_view name) const noexcept;
    [[nodiscard]] const ManifestBundleView*
    FindBundleByFileName(std::string_view file_name) const noexcept;

    [[nodiscard]] Result<void> VerifyContentHash() const;

  private:
    ManifestView(std::span<const std::byte> data,
                 std::shared_ptr<const std::vector<std::byte>> storage);

    [[nodiscard]] Result<void> Parse(bool verify_content_hash);

    std::span<const std::byte> data_;
    std::shared_ptr<const std::vector<std::byte>> storage_;
    ManifestHeader header_;
    std::string_view package_name_;
    std::string_view package_version_;
    std::vector<std::string_view> strings_;
    std::vector<std::string_view> tags_;
    std::vector<ManifestAssetView> assets_;
    std::vector<ManifestBundleView> bundles_;
    std::unordered_map<std::string_view, std::uint32_t> assets_by_address_;
    std::unordered_map<std::string_view, std::uint32_t> assets_by_path_;
    std::unordered_map<core::AssetGuid, std::uint32_t, core::Guid128Hash> assets_by_guid_;
    std::unordered_map<std::string_view, std::uint32_t> bundles_by_name_;
    std::unordered_map<std::string_view, std::uint32_t> bundles_by_file_name_;
    std::unordered_map<core::AssetGuid, std::uint32_t, core::Guid128Hash> bundles_by_guid_;
};

[[nodiscard]] Result<void> WriteManifestFile(std::string_view path, const ManifestDesc& desc,
                                             bool write_hash_sidecar = true);
[[nodiscard]] Result<ManifestView> LoadManifestFile(std::string_view path,
                                                    bool verify_content_hash = true);

} // namespace parcel::serialize
