#include <catch_amalgamated.hpp>

#include "parcel/serialize/asset_file.hpp"
#include "parcel/serialize/bundle.hpp"
#include "parcel/serialize/manifest.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using parcel::core::AssetGuid;
using parcel::core::Sha256;
using parcel::serialize::AssetFileDesc;
using parcel::serialize::AssetFileReader;
using parcel::serialize::AssetObjectDesc;
using parcel::serialize::AssetTypeDesc;
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

std::vector<std::byte> MakeAssetBytes() {
    AssetFileDesc desc;
    desc.types.push_back(AssetTypeDesc{1U, "parcel.FuzzAsset", 1U, 0U});
    desc.objects.push_back(AssetObjectDesc{1U, 1U, {std::byte{1}, std::byte{2}}, 0U, 0U});
    desc.blobs.push_back(parcel::serialize::AssetBlobDesc{{std::byte{3}, std::byte{4}}, 0U});
    auto bytes = WriteAssetFile(desc);
    REQUIRE(bytes.HasValue());
    return std::move(bytes).Value();
}

std::vector<std::byte> MakeBundleBytes() {
    BundleToc toc;
    toc.bundle_guid = Guid('1');
    toc.bundle_name = "fuzz.bundle";
    toc.payloads.push_back(parcel::serialize::BundlePayloadDesc{1U, 0U, 8U, Guid('2'), 0U});
    toc.raw_files.push_back(parcel::serialize::BundleRawFileDesc{"raw/a.txt", 8U, 4U, 0U});
    std::vector<std::byte> payload(12, std::byte{0x5A});
    auto bytes = WriteBundle(toc, payload);
    REQUIRE(bytes.HasValue());
    return std::move(bytes).Value();
}

std::vector<std::byte> MakeManifestBytes() {
    ManifestDesc desc;
    desc.package_name = "fuzz";
    desc.package_version = "1";
    ManifestBundleDesc bundle;
    bundle.bundle_guid = Guid('1');
    bundle.bundle_name = "b";
    bundle.file_name = "b.bundle";
    bundle.file_sha256 = Sha256("b");
    desc.bundles.push_back(bundle);
    ManifestAssetDesc asset;
    asset.asset_guid = Guid('2');
    asset.asset_path = "Assets/a.prefab";
    asset.type_name = "parcel.Fuzz";
    asset.bundle_id = 0U;
    asset.tags.push_back("tag");
    desc.assets.push_back(asset);
    auto bytes = WriteManifest(desc);
    REQUIRE(bytes.HasValue());
    return std::move(bytes).Value();
}

void ExerciseParsers(std::span<const std::byte> data) {
    (void)AssetFileReader::Open(data, false);
    (void)BundleReader::Open(data, false);
    (void)ManifestView::Open(data, false);
}

} // namespace

TEST_CASE("serialization parsers tolerate truncation and random bytes", "[serialize][fuzz]") {
    const std::vector<std::vector<std::byte>> seeds = {
        MakeAssetBytes(),
        MakeBundleBytes(),
        MakeManifestBytes(),
    };

    std::mt19937 generator(0xC0FFEEU);
    for (const auto& seed : seeds) {
        ExerciseParsers(seed);
        for (std::size_t attempt = 0; attempt < 250; ++attempt) {
            std::vector<std::byte> mutated = seed;
            const std::size_t operation_count = 1U + (generator() % 4U);
            for (std::size_t i = 0; i < operation_count; ++i) {
                if (mutated.empty()) {
                    break;
                }
                const std::size_t index = generator() % mutated.size();
                mutated[index] ^= static_cast<std::byte>(1U + (generator() % 255U));
            }
            if (attempt % 3U == 0U) {
                mutated.resize(generator() % (mutated.size() + 1U));
            }
            ExerciseParsers(mutated);
        }
    }

    for (std::size_t attempt = 0; attempt < 500; ++attempt) {
        std::vector<std::byte> random_bytes(static_cast<std::size_t>(generator() % 512U));
        for (auto& value : random_bytes) {
            value = static_cast<std::byte>(generator() & 0xFFU);
        }
        ExerciseParsers(random_bytes);
    }
}
