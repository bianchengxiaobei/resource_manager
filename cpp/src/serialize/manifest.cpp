#include "parcel/serialize/manifest.hpp"

#include "parcel/core/path.hpp"
#include "parcel/core/platform_file.hpp"
#include "parcel/serialize/binary.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace parcel::serialize {
namespace {

constexpr std::size_t kAssetRecordSize = 60;
constexpr std::size_t kBundleRecordSize = 96;

constexpr std::size_t kHeaderMagic = 0;
constexpr std::size_t kHeaderVersion = 4;
constexpr std::size_t kHeaderSize = 6;
constexpr std::size_t kHeaderFlags = 8;
constexpr std::size_t kHeaderFileSize = 12;
constexpr std::size_t kHeaderStringTableOffset = 20;
constexpr std::size_t kHeaderStringCount = 28;
constexpr std::size_t kHeaderAssetTableOffset = 32;
constexpr std::size_t kHeaderAssetCount = 40;
constexpr std::size_t kHeaderBundleTableOffset = 44;
constexpr std::size_t kHeaderBundleCount = 52;
constexpr std::size_t kHeaderTagTableOffset = 56;
constexpr std::size_t kHeaderTagCount = 64;
constexpr std::size_t kHeaderDependencyTableOffset = 68;
constexpr std::size_t kHeaderDependencyCount = 76;
constexpr std::size_t kHeaderPackageNameIndex = 80;
constexpr std::size_t kHeaderPackageVersionIndex = 84;
constexpr std::size_t kHeaderOutputNameStyle = 88;
constexpr std::size_t kHeaderBuildBundleType = 92;
constexpr std::size_t kHeaderContentHash = kManifestContentHashOffset;

constexpr std::size_t kAssetGuidOffset = 0;
constexpr std::size_t kAssetAddressIndex = 16;
constexpr std::size_t kAssetPathIndex = 20;
constexpr std::size_t kAssetTypeNameIndex = 24;
constexpr std::size_t kAssetBundleId = 28;
constexpr std::size_t kAssetPayloadId = 32;
constexpr std::size_t kAssetLocalObjectId = 36;
constexpr std::size_t kAssetDependencyOffset = 40;
constexpr std::size_t kAssetDependencyCount = 44;
constexpr std::size_t kAssetTagOffset = 48;
constexpr std::size_t kAssetTagCount = 52;
constexpr std::size_t kAssetFlags = 56;

constexpr std::size_t kBundleGuidOffset = 0;
constexpr std::size_t kBundleNameIndex = 16;
constexpr std::size_t kBundleFileNameIndex = 20;
constexpr std::size_t kBundleFileSize = 24;
constexpr std::size_t kBundleFileSha256 = 32;
constexpr std::size_t kBundleFileCrc = 64;
constexpr std::size_t kBundleFlags = 68;
constexpr std::size_t kBundleDependencyOffset = 72;
constexpr std::size_t kBundleDependencyCount = 76;
constexpr std::size_t kBundleReferrerOffset = 80;
constexpr std::size_t kBundleReferrerCount = 84;
constexpr std::size_t kBundleTagOffset = 88;
constexpr std::size_t kBundleTagCount = 92;

struct StringTable {
    std::map<std::string, std::uint32_t> indices;
    std::vector<std::string> values;

    std::uint32_t Add(std::string_view value) {
        const std::string key(value);
        const auto it = indices.find(key);
        if (it != indices.end()) {
            return it->second;
        }
        const auto index = static_cast<std::uint32_t>(values.size());
        indices.emplace(key, index);
        values.emplace_back(value);
        return index;
    }
};

struct AssetLayout {
    core::AssetGuid guid;
    std::uint32_t address_index = 0;
    std::uint32_t path_index = 0;
    std::uint32_t type_index = 0;
    std::uint32_t bundle_id = 0;
    std::uint32_t payload_id = 0;
    std::uint32_t local_object_id = 0;
    std::uint32_t dependency_offset = 0;
    std::uint32_t dependency_count = 0;
    std::uint32_t tag_offset = 0;
    std::uint32_t tag_count = 0;
    std::uint32_t flags = 0;
};

struct BundleLayout {
    core::AssetGuid guid;
    std::uint32_t name_index = 0;
    std::uint32_t file_name_index = 0;
    std::uint64_t file_size = 0;
    core::Hash256 file_sha256;
    std::uint32_t file_crc = 0;
    std::uint32_t flags = 0;
    std::uint32_t dependency_offset = 0;
    std::uint32_t dependency_count = 0;
    std::uint32_t referrer_offset = 0;
    std::uint32_t referrer_count = 0;
    std::uint32_t tag_offset = 0;
    std::uint32_t tag_count = 0;
};

struct RawAsset {
    core::AssetGuid guid;
    std::uint32_t address_index = 0;
    std::uint32_t path_index = 0;
    std::uint32_t type_index = 0;
    std::uint32_t bundle_id = 0;
    std::uint32_t payload_id = 0;
    std::uint32_t local_object_id = 0;
    std::uint32_t dependency_offset = 0;
    std::uint32_t dependency_count = 0;
    std::uint32_t tag_offset = 0;
    std::uint32_t tag_count = 0;
    std::uint32_t flags = 0;
};

struct RawBundle {
    core::AssetGuid guid;
    std::uint32_t name_index = 0;
    std::uint32_t file_name_index = 0;
    std::uint64_t file_size = 0;
    core::Hash256 file_sha256;
    std::uint32_t file_crc = 0;
    std::uint32_t flags = 0;
    std::uint32_t dependency_offset = 0;
    std::uint32_t dependency_count = 0;
    std::uint32_t referrer_offset = 0;
    std::uint32_t referrer_count = 0;
    std::uint32_t tag_offset = 0;
    std::uint32_t tag_count = 0;
};

[[nodiscard]] bool HasNul(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] Result<void> WriteHeader(BinaryWriter& writer, const ManifestHeader& header) {
    PARCEL_TRY(writer.PatchBytes(kHeaderMagic,
                                 std::as_bytes(std::span(kManifestMagic, sizeof(kManifestMagic)))));
    PARCEL_TRY(writer.PatchU16(kHeaderVersion, header.version));
    PARCEL_TRY(writer.PatchU16(kHeaderSize, header.header_size));
    PARCEL_TRY(writer.PatchU32(kHeaderFlags, header.flags));
    PARCEL_TRY(writer.PatchU64(kHeaderFileSize, header.file_size));
    PARCEL_TRY(writer.PatchU64(kHeaderStringTableOffset, header.string_table_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderStringCount, header.string_count));
    PARCEL_TRY(writer.PatchU64(kHeaderAssetTableOffset, header.asset_table_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderAssetCount, header.asset_count));
    PARCEL_TRY(writer.PatchU64(kHeaderBundleTableOffset, header.bundle_table_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderBundleCount, header.bundle_count));
    PARCEL_TRY(writer.PatchU64(kHeaderTagTableOffset, header.tag_table_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderTagCount, header.tag_count));
    PARCEL_TRY(writer.PatchU64(kHeaderDependencyTableOffset, header.dependency_table_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderDependencyCount, header.dependency_count));
    PARCEL_TRY(writer.PatchU32(kHeaderPackageNameIndex, header.package_name_index));
    PARCEL_TRY(writer.PatchU32(kHeaderPackageVersionIndex, header.package_version_index));
    PARCEL_TRY(writer.PatchU32(kHeaderOutputNameStyle, header.output_name_style));
    PARCEL_TRY(writer.PatchU32(kHeaderBuildBundleType, header.build_bundle_type));
    PARCEL_TRY(writer.PatchHash256(kHeaderContentHash, header.content_hash));
    return {};
}

[[nodiscard]] Result<void> ValidateManifest(const ManifestDesc& desc) {
    if (desc.package_name.empty() || desc.package_version.empty() || HasNul(desc.package_name) ||
        HasNul(desc.package_version)) {
        return core::MakeError(core::ErrorCode::InvalidArgument,
                               "package name and version are required");
    }
    if ((desc.flags & kManifestFlagReplaceAssetPathWithAddress) != 0 &&
        (desc.flags & kManifestFlagEnableAddressable) == 0) {
        return core::MakeError(core::ErrorCode::InvalidArgument,
                               "replace asset path requires addressable mode");
    }
    if ((desc.flags & kManifestFlagEnableAddressable) != 0 &&
        (desc.flags & kManifestFlagLocationToLower) != 0) {
        return core::MakeError(core::ErrorCode::InvalidArgument,
                               "addressable mode does not support lower-case locations");
    }
    if (desc.assets.size() > kMaxTableEntries || desc.bundles.size() > kMaxTableEntries ||
        desc.tags.size() > kMaxTableEntries) {
        return core::MakeError(core::ErrorCode::InvalidArgument,
                               "manifest table entry count exceeds the limit");
    }

    std::set<std::string> global_tags;
    for (const auto& tag : desc.tags) {
        if (tag.empty() || HasNul(tag) || !global_tags.insert(tag).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "invalid or duplicate global tag");
        }
    }

    std::set<core::AssetGuid> bundle_guids;
    std::set<std::string> bundle_names;
    std::set<std::string> bundle_files;
    for (std::size_t i = 0; i < desc.bundles.size(); ++i) {
        const auto& bundle = desc.bundles[i];
        if (bundle.bundle_guid.IsZero() || !bundle_guids.insert(bundle.bundle_guid).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "bundle GUID is zero or duplicated");
        }
        if (bundle.bundle_name.empty() || HasNul(bundle.bundle_name) ||
            !bundle_names.insert(bundle.bundle_name).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "bundle name is empty or duplicated");
        }
        if (bundle.file_name.empty() || HasNul(bundle.file_name) ||
            !core::path::IsSafeRelative(bundle.file_name) ||
            !bundle_files.insert(bundle.file_name).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "bundle file name is invalid or duplicated");
        }
        if (bundle.file_sha256.IsZero()) {
            return core::MakeError(core::ErrorCode::InvalidArgument, "bundle file hash is zero");
        }

        std::set<std::uint32_t> dependencies;
        for (std::uint32_t dependency : bundle.dependent_bundle_ids) {
            if (dependency >= desc.bundles.size() || dependency == i ||
                !dependencies.insert(dependency).second) {
                return core::MakeError(core::ErrorCode::InvalidArgument,
                                       "bundle dependency is invalid");
            }
        }
        std::set<std::uint32_t> referrers;
        for (std::uint32_t referrer : bundle.referrer_bundle_ids) {
            if (referrer >= desc.bundles.size() || referrer == i ||
                !referrers.insert(referrer).second) {
                return core::MakeError(core::ErrorCode::InvalidArgument,
                                       "bundle referrer is invalid");
            }
        }
        std::set<std::string> tags;
        for (const auto& tag : bundle.tags) {
            if (tag.empty() || HasNul(tag) || !tags.insert(tag).second) {
                return core::MakeError(core::ErrorCode::InvalidArgument,
                                       "invalid or duplicate bundle tag");
            }
        }
    }

    std::set<core::AssetGuid> asset_guids;
    std::set<std::string> asset_addresses;
    std::set<std::string> asset_paths;
    for (const auto& asset : desc.assets) {
        if (asset.asset_guid.IsZero() || !asset_guids.insert(asset.asset_guid).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "asset GUID is zero or duplicated");
        }
        if (asset.asset_path.empty() || HasNul(asset.asset_path) ||
            !core::path::IsSafeRelative(asset.asset_path) ||
            !asset_paths.insert(asset.asset_path).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "asset path is invalid or duplicated");
        }
        if (asset.type_name.empty() || HasNul(asset.type_name)) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "asset type name is empty or contains NUL");
        }
        if (asset.bundle_id >= desc.bundles.size()) {
            return core::MakeError(core::ErrorCode::InvalidArgument, "asset bundle id is invalid");
        }
        if ((desc.flags & kManifestFlagEnableAddressable) != 0 &&
            (asset.address.empty() || HasNul(asset.address))) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "asset address is required in addressable mode");
        }
        if (!asset.address.empty() && !asset_addresses.insert(asset.address).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument, "asset address is duplicated");
        }
        std::set<std::uint32_t> dependencies;
        for (std::uint32_t dependency : asset.dependent_bundle_ids) {
            if (dependency >= desc.bundles.size() || !dependencies.insert(dependency).second) {
                return core::MakeError(core::ErrorCode::InvalidArgument,
                                       "asset dependent bundle is invalid");
            }
        }
        std::set<std::string> tags;
        for (const auto& tag : asset.tags) {
            if (tag.empty() || HasNul(tag) || !tags.insert(tag).second) {
                return core::MakeError(core::ErrorCode::InvalidArgument,
                                       "invalid or duplicate asset tag");
            }
        }
    }
    return {};
}

[[nodiscard]] Result<std::vector<std::byte>> BuildStringTable(const StringTable& strings) {
    BinaryWriter writer(256);
    if (strings.values.size() > std::numeric_limits<std::uint32_t>::max()) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "string table is too large");
    }
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(strings.values.size())));
    for (const auto& value : strings.values) {
        PARCEL_TRY(writer.WriteString(value));
    }
    return writer.Release();
}

[[nodiscard]] Result<std::vector<std::byte>>
BuildAssetTable(const std::vector<AssetLayout>& assets) {
    BinaryWriter writer(static_cast<std::size_t>(assets.size()) * kAssetRecordSize);
    for (const auto& asset : assets) {
        PARCEL_TRY(writer.WriteGuid128(asset.guid));
        PARCEL_TRY(writer.WriteU32(asset.address_index));
        PARCEL_TRY(writer.WriteU32(asset.path_index));
        PARCEL_TRY(writer.WriteU32(asset.type_index));
        PARCEL_TRY(writer.WriteU32(asset.bundle_id));
        PARCEL_TRY(writer.WriteU32(asset.payload_id));
        PARCEL_TRY(writer.WriteU32(asset.local_object_id));
        PARCEL_TRY(writer.WriteU32(asset.dependency_offset));
        PARCEL_TRY(writer.WriteU32(asset.dependency_count));
        PARCEL_TRY(writer.WriteU32(asset.tag_offset));
        PARCEL_TRY(writer.WriteU32(asset.tag_count));
        PARCEL_TRY(writer.WriteU32(asset.flags));
    }
    return writer.Release();
}

[[nodiscard]] Result<std::vector<std::byte>>
BuildBundleTable(const std::vector<BundleLayout>& bundles) {
    BinaryWriter writer(static_cast<std::size_t>(bundles.size()) * kBundleRecordSize);
    for (const auto& bundle : bundles) {
        PARCEL_TRY(writer.WriteGuid128(bundle.guid));
        PARCEL_TRY(writer.WriteU32(bundle.name_index));
        PARCEL_TRY(writer.WriteU32(bundle.file_name_index));
        PARCEL_TRY(writer.WriteU64(bundle.file_size));
        PARCEL_TRY(writer.WriteHash256(bundle.file_sha256));
        PARCEL_TRY(writer.WriteU32(bundle.file_crc));
        PARCEL_TRY(writer.WriteU32(bundle.flags));
        PARCEL_TRY(writer.WriteU32(bundle.dependency_offset));
        PARCEL_TRY(writer.WriteU32(bundle.dependency_count));
        PARCEL_TRY(writer.WriteU32(bundle.referrer_offset));
        PARCEL_TRY(writer.WriteU32(bundle.referrer_count));
        PARCEL_TRY(writer.WriteU32(bundle.tag_offset));
        PARCEL_TRY(writer.WriteU32(bundle.tag_count));
    }
    return writer.Release();
}

[[nodiscard]] Result<std::vector<std::byte>>
BuildU32Table(const std::vector<std::uint32_t>& values) {
    BinaryWriter writer(values.size() * sizeof(std::uint32_t));
    for (std::uint32_t value : values) {
        PARCEL_TRY(writer.WriteU32(value));
    }
    return writer.Release();
}

} // namespace

Result<std::vector<std::byte>> WriteManifest(const ManifestDesc& desc) {
    PARCEL_TRY(ValidateManifest(desc));

    StringTable strings;
    const std::uint32_t package_name_index = strings.Add(desc.package_name);
    const std::uint32_t package_version_index = strings.Add(desc.package_version);

    std::vector<AssetLayout> assets;
    assets.reserve(desc.assets.size());
    for (const auto& asset : desc.assets) {
        AssetLayout layout;
        layout.guid = asset.asset_guid;
        layout.address_index = strings.Add(asset.address);
        layout.path_index = strings.Add(asset.asset_path);
        layout.type_index = strings.Add(asset.type_name);
        layout.bundle_id = asset.bundle_id;
        layout.payload_id = asset.payload_id;
        layout.local_object_id = asset.local_object_id;
        layout.flags = asset.flags;
        assets.push_back(layout);
    }

    std::vector<BundleLayout> bundles;
    bundles.reserve(desc.bundles.size());
    for (const auto& bundle : desc.bundles) {
        BundleLayout layout;
        layout.guid = bundle.bundle_guid;
        layout.name_index = strings.Add(bundle.bundle_name);
        layout.file_name_index = strings.Add(bundle.file_name);
        layout.file_size = bundle.file_size;
        layout.file_sha256 = bundle.file_sha256;
        layout.file_crc = bundle.file_crc;
        layout.flags = bundle.flags;
        bundles.push_back(layout);
    }

    std::vector<std::uint32_t> all_dependencies;
    for (std::size_t i = 0; i < desc.assets.size(); ++i) {
        auto& layout = assets[i];
        layout.dependency_offset = static_cast<std::uint32_t>(all_dependencies.size());
        layout.dependency_count =
            static_cast<std::uint32_t>(desc.assets[i].dependent_bundle_ids.size());
        all_dependencies.insert(all_dependencies.end(), desc.assets[i].dependent_bundle_ids.begin(),
                                desc.assets[i].dependent_bundle_ids.end());
    }
    for (std::size_t i = 0; i < desc.bundles.size(); ++i) {
        auto& layout = bundles[i];
        layout.dependency_offset = static_cast<std::uint32_t>(all_dependencies.size());
        layout.dependency_count =
            static_cast<std::uint32_t>(desc.bundles[i].dependent_bundle_ids.size());
        all_dependencies.insert(all_dependencies.end(),
                                desc.bundles[i].dependent_bundle_ids.begin(),
                                desc.bundles[i].dependent_bundle_ids.end());
        layout.referrer_offset = static_cast<std::uint32_t>(all_dependencies.size());
        layout.referrer_count =
            static_cast<std::uint32_t>(desc.bundles[i].referrer_bundle_ids.size());
        all_dependencies.insert(all_dependencies.end(), desc.bundles[i].referrer_bundle_ids.begin(),
                                desc.bundles[i].referrer_bundle_ids.end());
    }

    std::vector<std::uint32_t> tag_table;
    for (const auto& tag : desc.tags) {
        tag_table.push_back(strings.Add(tag));
    }
    for (std::size_t i = 0; i < desc.assets.size(); ++i) {
        assets[i].tag_offset = static_cast<std::uint32_t>(tag_table.size());
        assets[i].tag_count = static_cast<std::uint32_t>(desc.assets[i].tags.size());
        for (const auto& tag : desc.assets[i].tags) {
            tag_table.push_back(strings.Add(tag));
        }
    }
    for (std::size_t i = 0; i < desc.bundles.size(); ++i) {
        bundles[i].tag_offset = static_cast<std::uint32_t>(tag_table.size());
        bundles[i].tag_count = static_cast<std::uint32_t>(desc.bundles[i].tags.size());
        for (const auto& tag : desc.bundles[i].tags) {
            tag_table.push_back(strings.Add(tag));
        }
    }

    if (strings.values.size() > std::numeric_limits<std::uint32_t>::max() ||
        all_dependencies.size() > std::numeric_limits<std::uint32_t>::max() ||
        tag_table.size() > std::numeric_limits<std::uint32_t>::max()) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "manifest table is too large");
    }

    auto string_table = BuildStringTable(strings);
    if (!string_table) {
        return std::move(string_table).TakeError();
    }
    auto asset_table = BuildAssetTable(assets);
    if (!asset_table) {
        return std::move(asset_table).TakeError();
    }
    auto bundle_table = BuildBundleTable(bundles);
    if (!bundle_table) {
        return std::move(bundle_table).TakeError();
    }
    auto tag_table_bytes = BuildU32Table(tag_table);
    if (!tag_table_bytes) {
        return std::move(tag_table_bytes).TakeError();
    }
    auto dependency_table = BuildU32Table(all_dependencies);
    if (!dependency_table) {
        return std::move(dependency_table).TakeError();
    }

    BinaryWriter writer(256 + string_table.Value().size() + asset_table.Value().size() +
                        bundle_table.Value().size() + tag_table_bytes.Value().size() +
                        dependency_table.Value().size());
    PARCEL_TRY(writer.WriteZero(kManifestHeaderSize));

    ManifestHeader header;
    header.flags = desc.flags;
    header.string_table_offset = writer.Tell();
    header.string_count = static_cast<std::uint32_t>(strings.values.size());
    PARCEL_TRY(writer.WriteBytes(string_table.Value()));

    PARCEL_TRY(writer.Align(8));
    header.asset_table_offset = writer.Tell();
    header.asset_count = static_cast<std::uint32_t>(assets.size());
    PARCEL_TRY(writer.WriteBytes(asset_table.Value()));

    PARCEL_TRY(writer.Align(8));
    header.bundle_table_offset = writer.Tell();
    header.bundle_count = static_cast<std::uint32_t>(bundles.size());
    PARCEL_TRY(writer.WriteBytes(bundle_table.Value()));

    PARCEL_TRY(writer.Align(8));
    header.tag_table_offset = writer.Tell();
    header.tag_count = static_cast<std::uint32_t>(tag_table.size());
    PARCEL_TRY(writer.WriteBytes(tag_table_bytes.Value()));

    PARCEL_TRY(writer.Align(8));
    header.dependency_table_offset = writer.Tell();
    header.dependency_count = static_cast<std::uint32_t>(all_dependencies.size());
    PARCEL_TRY(writer.WriteBytes(dependency_table.Value()));

    header.package_name_index = package_name_index;
    header.package_version_index = package_version_index;
    header.output_name_style = desc.output_name_style;
    header.build_bundle_type = desc.build_bundle_type;
    header.file_size = writer.Size();
    header.content_hash = core::Sha256(writer.Data().subspan(kManifestHeaderSize));
    PARCEL_TRY(WriteHeader(writer, header));
    return writer.Release();
}

Result<std::vector<std::byte>> ManifestWriter::Build() const { return WriteManifest(desc_); }

ManifestView::ManifestView(std::span<const std::byte> data,
                           std::shared_ptr<const std::vector<std::byte>> storage)
    : data_(data), storage_(std::move(storage)) {}

Result<ManifestView> ManifestView::Open(std::span<const std::byte> data, bool verify_content_hash) {
    ManifestView view(data, {});
    PARCEL_TRY(view.Parse(verify_content_hash));
    return view;
}

Result<ManifestView> ManifestView::Open(std::shared_ptr<const std::vector<std::byte>> storage,
                                        bool verify_content_hash) {
    if (!storage) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "manifest storage is null");
    }
    const std::span<const std::byte> data(*storage);
    ManifestView view(data, std::move(storage));
    PARCEL_TRY(view.Parse(verify_content_hash));
    return view;
}

Result<void> ManifestView::Parse(bool verify_content_hash) {
    if (data_.size() < kManifestHeaderSize) {
        return core::MakeError(core::ErrorCode::Corrupt, "manifest file is too small");
    }

    BinaryReader reader(data_.first(kManifestHeaderSize));
    auto magic = reader.ReadBytes(4);
    if (!magic) {
        return std::move(magic).TakeError();
    }
    if (std::memcmp(magic.Value().data(), kManifestMagic, sizeof(kManifestMagic)) != 0) {
        return core::MakeError(core::ErrorCode::Corrupt, "invalid manifest file magic");
    }
    PARCEL_TRY_DECL(std::uint16_t, version, reader.ReadU16());
    PARCEL_TRY_DECL(std::uint16_t, header_size, reader.ReadU16());
    PARCEL_TRY_DECL(std::uint32_t, flags, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, file_size, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint64_t, string_table_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, string_count, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, asset_table_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, asset_count, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, bundle_table_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, bundle_count, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, tag_table_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, tag_count, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, dependency_table_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, dependency_count, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint32_t, package_name_index, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint32_t, package_version_index, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint32_t, output_name_style, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint32_t, build_bundle_type, reader.ReadU32());
    PARCEL_TRY_DECL(core::Hash256, content_hash, reader.ReadHash256());

    PARCEL_TRY(CheckVersion(version, kManifestVersion, "PRCM"));
    if (header_size != kManifestHeaderSize) {
        return core::MakeError(core::ErrorCode::Unsupported, "unsupported PRCM header size");
    }
    if (file_size != data_.size()) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               "PRCM file size does not match the header");
    }
    if (string_count > kMaxTableEntries || asset_count > kMaxTableEntries ||
        bundle_count > kMaxTableEntries || tag_count > kMaxTableEntries ||
        dependency_count > kMaxTableEntries) {
        return core::MakeError(core::ErrorCode::Corrupt, "manifest table is too large");
    }
    if ((flags & kManifestFlagReplaceAssetPathWithAddress) != 0 &&
        (flags & kManifestFlagEnableAddressable) == 0) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               "manifest replaces paths without addressable mode");
    }
    if ((flags & kManifestFlagEnableAddressable) != 0 &&
        (flags & kManifestFlagLocationToLower) != 0) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               "manifest addressable mode cannot lower-case locations");
    }
    if (string_table_offset < header_size || (tag_count != 0 && tag_table_offset < header_size) ||
        (dependency_count != 0 && dependency_table_offset < header_size)) {
        return core::MakeError(core::ErrorCode::Corrupt, "manifest table overlaps the header");
    }

    header_.version = version;
    header_.header_size = header_size;
    header_.flags = flags;
    header_.file_size = file_size;
    header_.string_table_offset = string_table_offset;
    header_.string_count = string_count;
    header_.asset_table_offset = asset_table_offset;
    header_.asset_count = asset_count;
    header_.bundle_table_offset = bundle_table_offset;
    header_.bundle_count = bundle_count;
    header_.tag_table_offset = tag_table_offset;
    header_.tag_count = tag_count;
    header_.dependency_table_offset = dependency_table_offset;
    header_.dependency_count = dependency_count;
    header_.package_name_index = package_name_index;
    header_.package_version_index = package_version_index;
    header_.output_name_style = output_name_style;
    header_.build_bundle_type = build_bundle_type;
    header_.content_hash = content_hash;

    PARCEL_TRY(CheckTableRange(file_size, asset_table_offset, asset_count, kAssetRecordSize, 1,
                               "manifest asset table"));
    PARCEL_TRY(CheckTableRange(file_size, bundle_table_offset, bundle_count, kBundleRecordSize, 1,
                               "manifest bundle table"));
    PARCEL_TRY(CheckTableRange(file_size, tag_table_offset, tag_count, sizeof(std::uint32_t), 1,
                               "manifest tag table"));
    PARCEL_TRY(CheckTableRange(file_size, dependency_table_offset, dependency_count,
                               sizeof(std::uint32_t), 1, "manifest dependency table"));
    if (string_table_offset >= file_size) {
        return core::MakeError(core::ErrorCode::Corrupt, "manifest string table offset is invalid");
    }

    BinaryReader string_reader(data_.subspan(static_cast<std::size_t>(string_table_offset)));
    PARCEL_TRY_DECL(std::uint32_t, stored_string_count, string_reader.ReadU32());
    if (stored_string_count != string_count) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               "manifest string count does not match the header");
    }
    strings_.reserve(string_count);
    for (std::uint32_t i = 0; i < string_count; ++i) {
        auto value = string_reader.ReadString();
        if (!value) {
            return std::move(value).TakeError();
        }
        if (HasNul(value.Value())) {
            return core::MakeError(core::ErrorCode::Corrupt, "manifest string contains NUL");
        }
        strings_.push_back(value.Value());
    }
    if (package_name_index >= strings_.size() || package_version_index >= strings_.size() ||
        strings_[package_name_index].empty() || strings_[package_version_index].empty()) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               "manifest package name or version index is invalid");
    }
    package_name_ = strings_[package_name_index];
    package_version_ = strings_[package_version_index];
    const ByteRange string_range{
        string_table_offset,
        static_cast<std::uint64_t>(string_reader.Position()),
    };

    std::vector<RawAsset> raw_assets;
    raw_assets.reserve(asset_count);
    std::set<core::AssetGuid> asset_guids;
    std::set<std::string> asset_addresses;
    std::set<std::string> asset_paths;
    for (std::uint32_t i = 0; i < asset_count; ++i) {
        const std::size_t offset = static_cast<std::size_t>(asset_table_offset) +
                                   static_cast<std::size_t>(i) * kAssetRecordSize;
        BinaryReader entry(data_.subspan(offset, kAssetRecordSize));
        RawAsset asset;
        PARCEL_TRY_ASSIGN(asset.guid, entry.ReadGuid128());
        PARCEL_TRY_ASSIGN(asset.address_index, entry.ReadU32());
        PARCEL_TRY_ASSIGN(asset.path_index, entry.ReadU32());
        PARCEL_TRY_ASSIGN(asset.type_index, entry.ReadU32());
        PARCEL_TRY_ASSIGN(asset.bundle_id, entry.ReadU32());
        PARCEL_TRY_ASSIGN(asset.payload_id, entry.ReadU32());
        PARCEL_TRY_ASSIGN(asset.local_object_id, entry.ReadU32());
        PARCEL_TRY_ASSIGN(asset.dependency_offset, entry.ReadU32());
        PARCEL_TRY_ASSIGN(asset.dependency_count, entry.ReadU32());
        PARCEL_TRY_ASSIGN(asset.tag_offset, entry.ReadU32());
        PARCEL_TRY_ASSIGN(asset.tag_count, entry.ReadU32());
        PARCEL_TRY_ASSIGN(asset.flags, entry.ReadU32());

        if (asset.guid.IsZero() || !asset_guids.insert(asset.guid).second) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "manifest asset GUID is zero or duplicated");
        }
        if (asset.bundle_id >= bundle_count) {
            return core::MakeError(core::ErrorCode::Corrupt, "manifest asset bundle id is invalid");
        }
        if (asset.address_index >= strings_.size() || asset.path_index >= strings_.size() ||
            asset.type_index >= strings_.size()) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "manifest asset string index is invalid");
        }
        if (asset.path_index >= strings_.size() || strings_[asset.path_index].empty() ||
            !core::path::IsSafeRelative(strings_[asset.path_index]) ||
            !asset_paths.insert(std::string(strings_[asset.path_index])).second) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "manifest asset path is invalid or duplicated");
        }
        if (strings_[asset.type_index].empty()) {
            return core::MakeError(core::ErrorCode::Corrupt, "manifest asset type name is empty");
        }
        if ((flags & kManifestFlagEnableAddressable) != 0 &&
            strings_[asset.address_index].empty()) {
            return core::MakeError(core::ErrorCode::Corrupt, "manifest asset address is missing");
        }
        if (!strings_[asset.address_index].empty() &&
            !asset_addresses.insert(std::string(strings_[asset.address_index])).second) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "manifest asset address is duplicated");
        }
        if (asset.dependency_count > dependency_count ||
            asset.dependency_offset > dependency_count - asset.dependency_count ||
            asset.tag_count > tag_count || asset.tag_offset > tag_count - asset.tag_count) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "manifest asset table range is invalid");
        }
        raw_assets.push_back(asset);
    }

    std::vector<RawBundle> raw_bundles;
    raw_bundles.reserve(bundle_count);
    std::set<core::AssetGuid> bundle_guids;
    std::set<std::string> bundle_names;
    std::set<std::string> bundle_files;
    for (std::uint32_t i = 0; i < bundle_count; ++i) {
        const std::size_t offset = static_cast<std::size_t>(bundle_table_offset) +
                                   static_cast<std::size_t>(i) * kBundleRecordSize;
        BinaryReader entry(data_.subspan(offset, kBundleRecordSize));
        RawBundle bundle;
        PARCEL_TRY_ASSIGN(bundle.guid, entry.ReadGuid128());
        PARCEL_TRY_ASSIGN(bundle.name_index, entry.ReadU32());
        PARCEL_TRY_ASSIGN(bundle.file_name_index, entry.ReadU32());
        PARCEL_TRY_ASSIGN(bundle.file_size, entry.ReadU64());
        PARCEL_TRY_ASSIGN(bundle.file_sha256, entry.ReadHash256());
        PARCEL_TRY_ASSIGN(bundle.file_crc, entry.ReadU32());
        PARCEL_TRY_ASSIGN(bundle.flags, entry.ReadU32());
        PARCEL_TRY_ASSIGN(bundle.dependency_offset, entry.ReadU32());
        PARCEL_TRY_ASSIGN(bundle.dependency_count, entry.ReadU32());
        PARCEL_TRY_ASSIGN(bundle.referrer_offset, entry.ReadU32());
        PARCEL_TRY_ASSIGN(bundle.referrer_count, entry.ReadU32());
        PARCEL_TRY_ASSIGN(bundle.tag_offset, entry.ReadU32());
        PARCEL_TRY_ASSIGN(bundle.tag_count, entry.ReadU32());

        if (bundle.guid.IsZero() || !bundle_guids.insert(bundle.guid).second) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "manifest bundle GUID is zero or duplicated");
        }
        if (bundle.name_index >= strings_.size() || bundle.file_name_index >= strings_.size() ||
            strings_[bundle.name_index].empty() || strings_[bundle.file_name_index].empty() ||
            !core::path::IsSafeRelative(strings_[bundle.file_name_index])) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "manifest bundle name or file name is invalid");
        }
        if (!bundle_names.insert(std::string(strings_[bundle.name_index])).second ||
            !bundle_files.insert(std::string(strings_[bundle.file_name_index])).second) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "manifest bundle name or file name is duplicated");
        }
        if (bundle.file_sha256.IsZero()) {
            return core::MakeError(core::ErrorCode::Corrupt, "manifest bundle file hash is zero");
        }
        if (bundle.dependency_count > dependency_count ||
            bundle.dependency_offset > dependency_count - bundle.dependency_count ||
            bundle.referrer_count > dependency_count ||
            bundle.referrer_offset > dependency_count - bundle.referrer_count ||
            bundle.tag_count > tag_count || bundle.tag_offset > tag_count - bundle.tag_count) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "manifest bundle table range is invalid");
        }
        raw_bundles.push_back(bundle);
    }

    std::vector<std::uint32_t> tag_table;
    tag_table.reserve(tag_count);
    std::set<std::string_view> unique_tags;
    for (std::uint32_t i = 0; i < tag_count; ++i) {
        const std::size_t offset = static_cast<std::size_t>(tag_table_offset) +
                                   static_cast<std::size_t>(i) * sizeof(std::uint32_t);
        BinaryReader entry(data_.subspan(offset, sizeof(std::uint32_t)));
        PARCEL_TRY_DECL(std::uint32_t, string_index, entry.ReadU32());
        if (string_index >= strings_.size() || strings_[string_index].empty()) {
            return core::MakeError(core::ErrorCode::Corrupt, "manifest tag table is invalid");
        }
        tag_table.push_back(string_index);
        if (unique_tags.insert(strings_[string_index]).second) {
            tags_.push_back(strings_[string_index]);
        }
    }

    std::vector<std::uint32_t> dependency_table;
    dependency_table.reserve(dependency_count);
    for (std::uint32_t i = 0; i < dependency_count; ++i) {
        const std::size_t offset = static_cast<std::size_t>(dependency_table_offset) +
                                   static_cast<std::size_t>(i) * sizeof(std::uint32_t);
        BinaryReader entry(data_.subspan(offset, sizeof(std::uint32_t)));
        PARCEL_TRY_DECL(std::uint32_t, bundle_id, entry.ReadU32());
        if (bundle_id >= bundle_count) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "manifest dependency bundle id is invalid");
        }
        dependency_table.push_back(bundle_id);
    }

    const auto resolve_tags = [&](std::uint32_t offset, std::uint32_t count) {
        std::vector<std::string_view> result;
        result.reserve(count);
        std::set<std::string_view> seen;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t string_index = tag_table[offset + i];
            const std::string_view tag = strings_[string_index];
            if (!seen.insert(tag).second) {
                return Result<std::vector<std::string_view>>(core::MakeError(
                    core::ErrorCode::Corrupt, "manifest tag is duplicated in a record"));
            }
            result.push_back(tag);
        }
        return Result<std::vector<std::string_view>>(std::move(result));
    };

    std::vector<ManifestAssetView> assets;
    assets.reserve(raw_assets.size());
    for (const auto& raw : raw_assets) {
        auto tags = resolve_tags(raw.tag_offset, raw.tag_count);
        if (!tags) {
            return std::move(tags).TakeError();
        }
        ManifestAssetView asset;
        asset.asset_guid = raw.guid;
        asset.address = strings_[raw.address_index];
        asset.asset_path = strings_[raw.path_index];
        asset.type_name = strings_[raw.type_index];
        asset.bundle_index = raw.bundle_id;
        asset.payload_id = raw.payload_id;
        asset.local_object_id = raw.local_object_id;
        asset.flags = raw.flags;
        asset.dependent_bundle_ids.assign(dependency_table.begin() + raw.dependency_offset,
                                          dependency_table.begin() + raw.dependency_offset +
                                              raw.dependency_count);
        std::set<std::uint32_t> asset_dependencies;
        for (std::uint32_t dependency : asset.dependent_bundle_ids) {
            if (!asset_dependencies.insert(dependency).second) {
                return core::MakeError(core::ErrorCode::Corrupt,
                                       "manifest asset dependency is duplicated");
            }
        }
        asset.tags = std::move(tags).Value();
        assets.push_back(std::move(asset));
    }

    std::vector<ManifestBundleView> bundles;
    bundles.reserve(raw_bundles.size());
    for (const auto& raw : raw_bundles) {
        auto tags = resolve_tags(raw.tag_offset, raw.tag_count);
        if (!tags) {
            return std::move(tags).TakeError();
        }
        ManifestBundleView bundle;
        bundle.bundle_guid = raw.guid;
        bundle.bundle_name = strings_[raw.name_index];
        bundle.file_name = strings_[raw.file_name_index];
        bundle.file_size = raw.file_size;
        bundle.file_sha256 = raw.file_sha256;
        bundle.file_crc = raw.file_crc;
        bundle.flags = raw.flags;
        bundle.dependent_bundle_ids.assign(dependency_table.begin() + raw.dependency_offset,
                                           dependency_table.begin() + raw.dependency_offset +
                                               raw.dependency_count);
        bundle.referrer_bundle_ids.assign(dependency_table.begin() + raw.referrer_offset,
                                          dependency_table.begin() + raw.referrer_offset +
                                              raw.referrer_count);
        bundle.tags = std::move(tags).Value();
        std::set<std::uint32_t> bundle_dependencies;
        std::set<std::uint32_t> bundle_referrers;
        for (std::uint32_t dependency : bundle.dependent_bundle_ids) {
            if (dependency >= bundle_count || dependency == bundles.size() ||
                !bundle_dependencies.insert(dependency).second) {
                return core::MakeError(core::ErrorCode::Corrupt,
                                       "manifest bundle dependency is invalid");
            }
        }
        for (std::uint32_t referrer : bundle.referrer_bundle_ids) {
            if (referrer >= bundle_count || referrer == bundles.size() ||
                !bundle_referrers.insert(referrer).second) {
                return core::MakeError(core::ErrorCode::Corrupt,
                                       "manifest bundle referrer is invalid");
            }
        }
        bundles.push_back(std::move(bundle));
    }

    for (const auto& asset : assets) {
        if (!assets_by_guid_
                 .emplace(asset.asset_guid, static_cast<std::uint32_t>(&asset - assets.data()))
                 .second) {
            return core::MakeError(core::ErrorCode::Corrupt, "manifest asset GUID is duplicated");
        }
        if (!asset.address.empty()) {
            assets_by_address_.emplace(asset.address,
                                       static_cast<std::uint32_t>(&asset - assets.data()));
        }
        assets_by_path_.emplace(asset.asset_path,
                                static_cast<std::uint32_t>(&asset - assets.data()));
    }
    for (const auto& bundle : bundles) {
        bundles_by_guid_.emplace(bundle.bundle_guid,
                                 static_cast<std::uint32_t>(&bundle - bundles.data()));
        bundles_by_name_.emplace(bundle.bundle_name,
                                 static_cast<std::uint32_t>(&bundle - bundles.data()));
        bundles_by_file_name_.emplace(bundle.file_name,
                                      static_cast<std::uint32_t>(&bundle - bundles.data()));
    }

    std::vector<ByteRange> ranges;
    ranges.push_back(ByteRange{0, header_size});
    ranges.push_back(string_range);
    ranges.push_back(ByteRange{asset_table_offset, asset_count * kAssetRecordSize});
    ranges.push_back(ByteRange{bundle_table_offset, bundle_count * kBundleRecordSize});
    ranges.push_back(ByteRange{tag_table_offset, tag_count * sizeof(std::uint32_t)});
    ranges.push_back(ByteRange{dependency_table_offset, dependency_count * sizeof(std::uint32_t)});
    PARCEL_TRY(CheckNoOverlap(ranges, "manifest table"));

    assets_ = std::move(assets);
    bundles_ = std::move(bundles);
    if (verify_content_hash) {
        PARCEL_TRY(VerifyContentHash());
    }
    return {};
}

Result<void> ManifestView::VerifyContentHash() const {
    if (data_.size() < kManifestHeaderSize) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "manifest is too small");
    }
    const core::Hash256 actual = core::Sha256(data_.subspan(kManifestHeaderSize));
    if (actual != header_.content_hash) {
        return core::MakeError(core::ErrorCode::HashMismatch, "manifest content hash mismatch");
    }
    return {};
}

const ManifestAssetView* ManifestView::FindAssetByGuid(const core::AssetGuid& guid) const noexcept {
    const auto it = assets_by_guid_.find(guid);
    return it == assets_by_guid_.end() ? nullptr : &assets_[it->second];
}

const ManifestAssetView* ManifestView::FindAssetByAddress(std::string_view address) const noexcept {
    const auto it = assets_by_address_.find(address);
    return it == assets_by_address_.end() ? nullptr : &assets_[it->second];
}

const ManifestAssetView* ManifestView::FindAssetByPath(std::string_view path) const noexcept {
    const auto it = assets_by_path_.find(path);
    return it == assets_by_path_.end() ? nullptr : &assets_[it->second];
}

const ManifestBundleView*
ManifestView::FindBundleByGuid(const core::AssetGuid& guid) const noexcept {
    const auto it = bundles_by_guid_.find(guid);
    return it == bundles_by_guid_.end() ? nullptr : &bundles_[it->second];
}

const ManifestBundleView* ManifestView::FindBundleByName(std::string_view name) const noexcept {
    const auto it = bundles_by_name_.find(name);
    return it == bundles_by_name_.end() ? nullptr : &bundles_[it->second];
}

const ManifestBundleView*
ManifestView::FindBundleByFileName(std::string_view file_name) const noexcept {
    const auto it = bundles_by_file_name_.find(file_name);
    return it == bundles_by_file_name_.end() ? nullptr : &bundles_[it->second];
}

Result<void> WriteManifestFile(std::string_view path, const ManifestDesc& desc,
                               bool write_hash_sidecar) {
    auto bytes = WriteManifest(desc);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }

    core::PlatformFile filesystem;
    auto file = filesystem.OpenWrite(path);
    if (!file) {
        return std::move(file).TakeError();
    }
    PARCEL_TRY(core::WriteAll(*file.Value(), bytes.Value()));
    PARCEL_TRY(file.Value()->Flush());
    file.Value()->Close();

    if (write_hash_sidecar) {
        std::string hash_path(path);
        hash_path += ".hash";
        const core::Hash256 hash =
            core::Sha256(std::span<const std::byte>(bytes.Value()).subspan(kManifestHeaderSize));
        const std::string hash_text = hash.ToHex() + "\n";
        auto hash_file = filesystem.OpenWrite(hash_path);
        if (!hash_file) {
            return std::move(hash_file).TakeError();
        }
        PARCEL_TRY(core::WriteAll(*hash_file.Value(),
                                  std::as_bytes(std::span(hash_text.data(), hash_text.size()))));
        PARCEL_TRY(hash_file.Value()->Flush());
        hash_file.Value()->Close();
    }
    return {};
}

Result<ManifestView> LoadManifestFile(std::string_view path, bool verify_content_hash) {
    core::PlatformFile filesystem;
    auto file = filesystem.OpenRead(path);
    if (!file) {
        return std::move(file).TakeError();
    }
    auto bytes = core::ReadAll(*file.Value());
    file.Value()->Close();
    if (!bytes) {
        return std::move(bytes).TakeError();
    }

    auto storage = std::make_shared<std::vector<std::byte>>(std::move(bytes).Value());
    auto view = ManifestView::Open(storage, verify_content_hash);
    if (!view) {
        return std::move(view).TakeError();
    }

    std::string hash_path(path);
    hash_path += ".hash";
    auto exists = filesystem.Exists(hash_path);
    if (!exists) {
        return std::move(exists).TakeError();
    }
    if (!exists.Value() && core::path::FileName(path) == "manifest.bytes") {
        const std::string normalized = core::path::Normalize(path);
        const std::size_t slash = normalized.find_last_of('/');
        const std::string alternate = slash == std::string::npos
                                          ? "manifest.hash"
                                          : normalized.substr(0, slash + 1) + "manifest.hash";
        exists = filesystem.Exists(alternate);
        if (!exists) {
            return std::move(exists).TakeError();
        }
        if (exists.Value()) {
            hash_path = alternate;
        }
    }
    if (exists.Value()) {
        auto hash_file = filesystem.OpenRead(hash_path);
        if (!hash_file) {
            return std::move(hash_file).TakeError();
        }
        auto hash_bytes = core::ReadAll(*hash_file.Value());
        hash_file.Value()->Close();
        if (!hash_bytes) {
            return std::move(hash_bytes).TakeError();
        }
        std::string hash_text(reinterpret_cast<const char*>(hash_bytes.Value().data()),
                              hash_bytes.Value().size());
        while (!hash_text.empty() && (hash_text.back() == '\n' || hash_text.back() == '\r' ||
                                      hash_text.back() == ' ' || hash_text.back() == '\t')) {
            hash_text.pop_back();
        }
        auto expected = core::Hash256::FromHex(hash_text);
        if (!expected) {
            return std::move(expected).TakeError();
        }
        if (expected.Value() != view.Value().Header().content_hash) {
            return core::MakeError(core::ErrorCode::HashMismatch, "manifest sidecar hash mismatch");
        }
    }
    return view;
}

} // namespace parcel::serialize
