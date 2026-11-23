#include <catch_amalgamated.hpp>

#include "parcel/serialize/binary.hpp"
#include "parcel/serialize/manifest.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using parcel::core::AssetGuid;
using parcel::core::Sha256;
using parcel::serialize::kManifestFlagEnableAddressable;
using parcel::serialize::kManifestFlagLocationToLower;
using parcel::serialize::kManifestFlagReplaceAssetPathWithAddress;
using parcel::serialize::kManifestMagic;
using parcel::serialize::LoadManifestFile;
using parcel::serialize::ManifestAssetDesc;
using parcel::serialize::ManifestBundleDesc;
using parcel::serialize::ManifestDesc;
using parcel::serialize::ManifestView;
using parcel::serialize::ManifestWriter;
using parcel::serialize::WriteManifest;
using parcel::serialize::WriteManifestFile;

namespace {

AssetGuid Guid(char value) {
    std::string text = "00112233445566778899aabbccddee0";
    text.push_back(value);
    return AssetGuid::FromHex(text).Value();
}

ManifestDesc MakeManifest() {
    ManifestDesc desc;
    desc.package_name = "demo-package";
    desc.package_version = "1.2.3";
    desc.flags = 0;
    desc.output_name_style = 2;
    desc.build_bundle_type = 1;
    desc.tags.push_back("global");

    ManifestBundleDesc first;
    first.bundle_guid = Guid('1');
    first.bundle_name = "bundle.first";
    first.file_name = "bundle.first.bundle";
    first.file_size = 12345;
    first.file_sha256 = Sha256("first-bundle");
    first.file_crc = 0x12345678U;
    first.dependent_bundle_ids.push_back(1);
    first.tags.push_back("shared");
    desc.bundles.push_back(first);

    ManifestBundleDesc second;
    second.bundle_guid = Guid('2');
    second.bundle_name = "bundle.second";
    second.file_name = "bundle.second.bundle";
    second.file_size = 777;
    second.file_sha256 = Sha256("second-bundle");
    second.referrer_bundle_ids.push_back(0);
    desc.bundles.push_back(second);

    ManifestAssetDesc asset;
    asset.asset_guid = Guid('a');
    asset.address = "address/first";
    asset.asset_path = "Assets/first.prefab";
    asset.type_name = "parcel.Scene";
    asset.bundle_id = 0;
    asset.payload_id = 3;
    asset.local_object_id = 5;
    asset.dependent_bundle_ids.push_back(1);
    asset.tags.push_back("red");
    asset.tags.push_back("blue");
    desc.assets.push_back(asset);

    asset = ManifestAssetDesc{};
    asset.asset_guid = Guid('b');
    asset.address = "address/second";
    asset.asset_path = "Assets/second.prefab";
    asset.type_name = "parcel.Texture";
    asset.bundle_id = 1;
    asset.payload_id = 4;
    asset.local_object_id = 6;
    asset.tags.push_back("red");
    desc.assets.push_back(asset);
    return desc;
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

} // namespace

TEST_CASE("PRCM round trip and zero-copy lookups", "[serialize][manifest]") {
    const ManifestDesc desc = MakeManifest();
    auto bytes = WriteManifest(desc);
    REQUIRE(bytes.HasValue());
    REQUIRE(bytes.Value().size() > 160);
    REQUIRE(std::memcmp(bytes.Value().data(), kManifestMagic, 4) == 0);

    auto view = ManifestView::Open(bytes.Value());
    REQUIRE(view.HasValue());
    const auto& manifest = view.Value();
    REQUIRE(manifest.Header().string_count > 0);
    REQUIRE(manifest.PackageName() == "demo-package");
    REQUIRE(manifest.PackageVersion() == "1.2.3");
    REQUIRE(manifest.Assets().size() == 2U);
    REQUIRE(manifest.Bundles().size() == 2U);
    REQUIRE(manifest.Assets()[0].asset_path == "Assets/first.prefab");
    REQUIRE(manifest.Assets()[0].bundle_index == 0U);
    REQUIRE(manifest.Assets()[0].dependent_bundle_ids.size() == 1U);
    REQUIRE(manifest.Assets()[0].tags.size() == 2U);
    REQUIRE(manifest.Bundles()[0].bundle_name == "bundle.first");
    REQUIRE(manifest.Bundles()[0].dependent_bundle_ids.size() == 1U);
    REQUIRE(manifest.Bundles()[1].referrer_bundle_ids.size() == 1U);
    REQUIRE(manifest.Bundles()[0].file_sha256 == Sha256("first-bundle"));

    const auto* asset = manifest.FindAssetByGuid(Guid('a'));
    REQUIRE(asset != nullptr);
    REQUIRE(asset->asset_path == "Assets/first.prefab");
    REQUIRE(manifest.FindAssetByAddress("address/second") != nullptr);
    REQUIRE(manifest.FindAssetByPath("Assets/first.prefab") != nullptr);
    REQUIRE(manifest.FindAssetByAddress("missing") == nullptr);
    REQUIRE(manifest.FindBundleByGuid(Guid('2')) != nullptr);
    REQUIRE(manifest.FindBundleByName("bundle.second") != nullptr);
    REQUIRE(manifest.FindBundleByFileName("bundle.first.bundle") != nullptr);
    REQUIRE(manifest.FindBundleByName("missing") == nullptr);

    REQUIRE(std::find(manifest.Tags().begin(), manifest.Tags().end(), std::string_view("global")) !=
            manifest.Tags().end());
    REQUIRE(std::find(manifest.Tags().begin(), manifest.Tags().end(), std::string_view("red")) !=
            manifest.Tags().end());

    const auto* raw = reinterpret_cast<const char*>(bytes.Value().data());
    REQUIRE(asset->asset_path.data() >= raw);
    REQUIRE(asset->asset_path.data() < raw + bytes.Value().size());
}

TEST_CASE("PRCM writer and reader reject invalid manifests", "[serialize][manifest]") {
    ManifestDesc desc = MakeManifest();
    desc.package_name.clear();
    REQUIRE_FALSE(WriteManifest(desc).HasValue());

    desc = MakeManifest();
    desc.assets[1].asset_guid = desc.assets[0].asset_guid;
    REQUIRE_FALSE(WriteManifest(desc).HasValue());

    desc = MakeManifest();
    desc.assets[1].asset_path = desc.assets[0].asset_path;
    REQUIRE_FALSE(WriteManifest(desc).HasValue());

    desc = MakeManifest();
    desc.assets[0].bundle_id = 99;
    REQUIRE_FALSE(WriteManifest(desc).HasValue());

    desc = MakeManifest();
    desc.assets[0].dependent_bundle_ids.push_back(99);
    REQUIRE_FALSE(WriteManifest(desc).HasValue());

    desc = MakeManifest();
    desc.assets[0].asset_path = "../escape.prefab";
    REQUIRE_FALSE(WriteManifest(desc).HasValue());

    desc = MakeManifest();
    desc.bundles[0].file_sha256 = {};
    REQUIRE_FALSE(WriteManifest(desc).HasValue());

    desc = MakeManifest();
    desc.flags = kManifestFlagReplaceAssetPathWithAddress;
    REQUIRE_FALSE(WriteManifest(desc).HasValue());

    desc = MakeManifest();
    desc.flags = kManifestFlagEnableAddressable | kManifestFlagLocationToLower;
    REQUIRE_FALSE(WriteManifest(desc).HasValue());
}

TEST_CASE("PRCM detects corruption and truncation", "[serialize][manifest]") {
    const ManifestDesc desc = MakeManifest();
    auto bytes = WriteManifest(desc);
    REQUIRE(bytes.HasValue());

    auto bad_magic = bytes.Value();
    bad_magic[0] = std::byte{'X'};
    REQUIRE_FALSE(ManifestView::Open(bad_magic).HasValue());

    auto bad_hash = bytes.Value();
    bad_hash[96] ^= std::byte{0xFF};
    REQUIRE_FALSE(ManifestView::Open(bad_hash).HasValue());

    auto bad_version = bytes.Value();
    bad_version[4] = std::byte{99};
    bad_version[5] = std::byte{0};
    REQUIRE_FALSE(ManifestView::Open(bad_version, false).HasValue());

    auto truncated = bytes.Value();
    truncated.resize(100);
    REQUIRE_FALSE(ManifestView::Open(truncated).HasValue());

    auto bad_table = bytes.Value();
    {
        auto valid = ManifestView::Open(bad_table);
        REQUIRE(valid.HasValue());
        PatchU64(bad_table, 32, static_cast<std::uint64_t>(bad_table.size()) + 4096U);
    }
    REQUIRE_FALSE(ManifestView::Open(bad_table, false).HasValue());

    auto duplicate_guid = bytes.Value();
    {
        auto valid = ManifestView::Open(duplicate_guid);
        REQUIRE(valid.HasValue());
        const std::size_t asset_table =
            static_cast<std::size_t>(valid.Value().Header().asset_table_offset);
        std::copy(duplicate_guid.begin() + static_cast<std::ptrdiff_t>(asset_table),
                  duplicate_guid.begin() + static_cast<std::ptrdiff_t>(asset_table + 16),
                  duplicate_guid.begin() + static_cast<std::ptrdiff_t>(asset_table + 60));
    }
    REQUIRE_FALSE(ManifestView::Open(duplicate_guid, false).HasValue());

    auto bad_dependency = bytes.Value();
    {
        auto valid = ManifestView::Open(bad_dependency);
        REQUIRE(valid.HasValue());
        const std::uint64_t table = valid.Value().Header().dependency_table_offset;
        PatchU32(bad_dependency, static_cast<std::size_t>(table), 99U);
    }
    REQUIRE_FALSE(ManifestView::Open(bad_dependency, false).HasValue());
}

TEST_CASE("PRCM manifest files and sidecar hash round trip", "[serialize][manifest]") {
    parcel::test::TempDir temp("manifest");
    const auto path = temp.Path() / "package.prcm";
    const std::string path_utf8 = parcel::test::ToUtf8(path);
    const ManifestDesc desc = MakeManifest();

    REQUIRE(WriteManifestFile(path_utf8, desc).HasValue());
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(std::filesystem::exists(path_utf8 + ".hash"));

    auto loaded = LoadManifestFile(path_utf8);
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value().Assets().size() == 2U);
    REQUIRE(loaded.Value().FindAssetByGuid(Guid('b')) != nullptr);

    std::string sidecar = path_utf8 + ".hash";
    {
        std::ofstream stream(sidecar, std::ios::binary | std::ios::trunc);
        REQUIRE(stream.good());
        stream << "deadbeef\n";
    }
    REQUIRE_FALSE(LoadManifestFile(path_utf8).HasValue());
}

TEST_CASE("PRCM handles large asset and bundle tables", "[serialize][manifest][perf]") {
    ManifestDesc desc;
    desc.package_name = "perf";
    desc.package_version = "1.0.0";
    for (std::uint32_t i = 0; i < 1000; ++i) {
        ManifestBundleDesc bundle;
        bundle.bundle_guid = Guid('1');
        bundle.bundle_guid.bytes[12] = static_cast<std::uint8_t>(i >> 16U);
        bundle.bundle_guid.bytes[13] = static_cast<std::uint8_t>(i >> 8U);
        bundle.bundle_guid.bytes[14] = static_cast<std::uint8_t>(i);
        bundle.bundle_guid.bytes[15] = static_cast<std::uint8_t>((i * 17U) & 0xFFU);
        bundle.bundle_name = "bundle_" + std::to_string(i);
        bundle.file_name = "bundle_" + std::to_string(i) + ".bundle";
        bundle.file_sha256 = Sha256(std::to_string(i));
        desc.bundles.push_back(bundle);
    }
    for (std::uint32_t i = 0; i < 10000; ++i) {
        ManifestAssetDesc asset;
        asset.asset_guid = Guid('0');
        asset.asset_guid.bytes[12] = static_cast<std::uint8_t>(i >> 16U);
        asset.asset_guid.bytes[13] = static_cast<std::uint8_t>(i >> 8U);
        asset.asset_guid.bytes[14] = static_cast<std::uint8_t>(i);
        asset.asset_guid.bytes[15] = static_cast<std::uint8_t>((i * 31U) & 0xFFU);
        asset.address = "address/" + std::to_string(i);
        asset.asset_path = "Assets/asset_" + std::to_string(i) + ".prefab";
        asset.type_name = "parcel.Test";
        asset.bundle_id = i % desc.bundles.size();
        asset.dependent_bundle_ids.push_back((i + 1) % desc.bundles.size());
        desc.assets.push_back(asset);
    }

    auto bytes = WriteManifest(desc);
    REQUIRE(bytes.HasValue());
    auto view = ManifestView::Open(bytes.Value());
    REQUIRE(view.HasValue());
    REQUIRE(view.Value().Assets().size() == 10000U);
    REQUIRE(view.Value().Bundles().size() == 1000U);
    REQUIRE(view.Value().FindAssetByAddress("address/9999") != nullptr);
}
