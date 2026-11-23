#include <catch_amalgamated.hpp>

#include "parcel/serialize/asset_file.hpp"
#include "parcel/serialize/bundle.hpp"
#include "parcel/serialize/format.hpp"
#include "parcel/serialize/manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using parcel::core::AssetGuid;
using parcel::core::Sha256;
using parcel::serialize::AssetFileDesc;
using parcel::serialize::AssetFileReader;
using parcel::serialize::AssetObjectDesc;
using parcel::serialize::AssetTypeDesc;
using parcel::serialize::BundlePayloadDesc;
using parcel::serialize::BundleReader;
using parcel::serialize::BundleToc;
using parcel::serialize::ManifestAssetDesc;
using parcel::serialize::ManifestBundleDesc;
using parcel::serialize::ManifestDesc;
using parcel::serialize::ManifestView;
using parcel::serialize::WriteAssetFile;
using parcel::serialize::WriteBundle;
using parcel::serialize::WriteManifest;

namespace {

AssetGuid Guid(char value) {
    std::string text = "00112233445566778899aabbccddee0";
    text.push_back(value);
    return AssetGuid::FromHex(text).Value();
}

void PatchU32(std::vector<std::byte>& data, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
    }
}

void PatchU64(std::vector<std::byte>& data, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        data[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
    }
}

std::vector<std::byte> MakeManifestBytes() {
    ManifestDesc desc;
    desc.package_name = "security";
    desc.package_version = "1.0.0";
    ManifestBundleDesc bundle;
    bundle.bundle_guid = Guid('1');
    bundle.bundle_name = "bundle";
    bundle.file_name = "bundle.bundle";
    bundle.file_sha256 = Sha256("bundle");
    desc.bundles.push_back(bundle);
    ManifestAssetDesc asset;
    asset.asset_guid = Guid('2');
    asset.address = "asset";
    asset.asset_path = "Assets/asset.prefab";
    asset.type_name = "parcel.SecurityAsset";
    desc.assets.push_back(asset);
    auto bytes = WriteManifest(desc);
    REQUIRE(bytes.HasValue());
    return std::move(bytes).Value();
}

std::vector<std::byte> MakeBundleBytes() {
    BundleToc toc;
    toc.bundle_guid = Guid('3');
    toc.bundle_name = "security";
    toc.payloads.push_back(BundlePayloadDesc{1U, 0U, 8192U, Guid('4'), 0U});
    std::vector<std::byte> payload(8192U, std::byte{0x3C});
    auto bytes = WriteBundle(toc, payload);
    REQUIRE(bytes.HasValue());
    return std::move(bytes).Value();
}

} // namespace

TEST_CASE("PRCM rejects oversized declared tables", "[serialize][security]") {
    auto bytes = MakeManifestBytes();
    auto huge_strings = bytes;
    PatchU32(huge_strings, 28, parcel::serialize::kMaxTableEntries + 1U);
    REQUIRE_FALSE(ManifestView::Open(huge_strings, false).HasValue());

    auto huge_assets = bytes;
    PatchU32(huge_assets, 40, parcel::serialize::kMaxTableEntries + 1U);
    REQUIRE_FALSE(ManifestView::Open(huge_assets, false).HasValue());

    auto huge_bundles = bytes;
    PatchU32(huge_bundles, 52, parcel::serialize::kMaxTableEntries + 1U);
    REQUIRE_FALSE(ManifestView::Open(huge_bundles, false).HasValue());

    auto huge_dependencies = bytes;
    PatchU32(huge_dependencies, 76, parcel::serialize::kMaxTableEntries + 1U);
    REQUIRE_FALSE(ManifestView::Open(huge_dependencies, false).HasValue());
}

TEST_CASE("PRCA rejects oversized declared tables", "[serialize][security]") {
    AssetFileDesc desc;
    desc.types.push_back(AssetTypeDesc{1U, "parcel.SecurityAsset", 1U, 0U});
    AssetObjectDesc object;
    object.local_id = 1U;
    object.type_id = 1U;
    object.metadata.assign(8U, std::byte{1});
    desc.objects.push_back(object);
    auto result = WriteAssetFile(desc);
    REQUIRE(result.HasValue());
    auto bytes = std::move(result).Value();

    auto huge_types = bytes;
    PatchU32(huge_types, 28, parcel::serialize::kMaxTableEntries + 1U);
    REQUIRE_FALSE(AssetFileReader::Open(huge_types, false).HasValue());

    auto huge_objects = bytes;
    PatchU32(huge_objects, 40, parcel::serialize::kMaxTableEntries + 1U);
    REQUIRE_FALSE(AssetFileReader::Open(huge_objects, false).HasValue());

    auto huge_blobs = bytes;
    PatchU32(huge_blobs, 88, parcel::serialize::kMaxTableEntries + 1U);
    REQUIRE_FALSE(AssetFileReader::Open(huge_blobs, false).HasValue());
}

TEST_CASE("PRCL rejects oversized block and TOC declarations", "[serialize][security]") {
    auto bytes = MakeBundleBytes();
    auto huge_blocks = bytes;
    PatchU32(huge_blocks, 32, parcel::serialize::kMaxTableEntries + 1U);
    REQUIRE_FALSE(BundleReader::Open(huge_blocks, false).HasValue());

    auto huge_block_size = bytes;
    PatchU32(huge_block_size, 52, parcel::serialize::kMaxBlockSize + 1U);
    REQUIRE_FALSE(BundleReader::Open(huge_block_size, false).HasValue());

    auto huge_toc = bytes;
    PatchU32(huge_toc, 48, parcel::serialize::kMaxTocRawSize + 1U);
    REQUIRE_FALSE(BundleReader::Open(huge_toc, false).HasValue());

    auto huge_compressed_toc = bytes;
    PatchU32(huge_compressed_toc, 44, parcel::serialize::kMaxTocRawSize + 1U);
    REQUIRE_FALSE(BundleReader::Open(huge_compressed_toc, false).HasValue());
}

TEST_CASE("PRCL rejects compressed and logical block bombs", "[serialize][security]") {
    auto bytes = MakeBundleBytes();
    auto header_reader = BundleReader::Open(bytes, false);
    REQUIRE(header_reader.HasValue());
    const std::uint64_t block_table = header_reader.Value().Header().block_table_offset;
    const std::uint32_t block_size = header_reader.Value().Header().block_size;

    auto oversized_compressed = bytes;
    PatchU32(oversized_compressed, static_cast<std::size_t>(block_table) + 8U,
             parcel::serialize::kMaxCompressedBlockSize + 1U);
    BundleReader compressed_reader(
        std::span<const std::byte>(oversized_compressed.data(), oversized_compressed.size()));
    REQUIRE(compressed_reader.ReadHeader().HasValue());
    REQUIRE_FALSE(compressed_reader.ReadBlocks().HasValue());

    auto oversized_logical = bytes;
    PatchU32(oversized_logical, static_cast<std::size_t>(block_table) + 12U, block_size + 1U);
    BundleReader logical_reader(
        std::span<const std::byte>(oversized_logical.data(), oversized_logical.size()));
    REQUIRE(logical_reader.ReadHeader().HasValue());
    REQUIRE_FALSE(logical_reader.ReadBlocks().HasValue());
}

TEST_CASE("PRCM rejects invalid string and range references", "[serialize][security]") {
    auto bytes = MakeManifestBytes();
    auto reader = ManifestView::Open(bytes, false);
    REQUIRE(reader.HasValue());
    const std::uint64_t asset_table = reader.Value().Header().asset_table_offset;
    const std::uint32_t string_count = reader.Value().Header().string_count;

    auto huge_string = bytes;
    PatchU32(huge_string, static_cast<std::size_t>(asset_table) + 16U, string_count + 1000U);
    REQUIRE_FALSE(ManifestView::Open(huge_string, false).HasValue());

    auto path_escape = bytes;
    PatchU32(path_escape, static_cast<std::size_t>(asset_table) + 20U, string_count + 1000U);
    REQUIRE_FALSE(ManifestView::Open(path_escape, false).HasValue());
}
