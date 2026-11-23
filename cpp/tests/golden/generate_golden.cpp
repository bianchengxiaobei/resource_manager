#include "parcel/serialize/asset_file.hpp"
#include "parcel/serialize/bundle.hpp"
#include "parcel/serialize/manifest.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

parcel::core::AssetGuid Guid(char value) {
    std::string text = "00112233445566778899aabbccddee0";
    text.push_back(value);
    return parcel::core::AssetGuid::FromHex(text).Value();
}

void WriteBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void WriteAsset(const std::filesystem::path& path) {
    parcel::serialize::AssetFileDesc desc;
    desc.types.push_back(parcel::serialize::AssetTypeDesc{1U, "parcel.GoldenAsset", 1U, 0U});
    desc.objects.push_back(parcel::serialize::AssetObjectDesc{
        10U, 1U, {std::byte{1}, std::byte{2}, std::byte{3}}, 0U, 0U});
    desc.external_refs.push_back(parcel::serialize::AssetExternalRefDesc{Guid('2'), 0U});
    desc.sub_assets.push_back(parcel::serialize::AssetSubAssetDesc{"sub", 10U, 0U});
    desc.blobs.push_back(
        parcel::serialize::AssetBlobDesc{{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}}, 0U});
    auto bytes = parcel::serialize::WriteAssetFile(desc);
    if (!bytes) {
        std::abort();
    }
    WriteBytes(path, bytes.Value());
}

void WriteBundleFile(const std::filesystem::path& path) {
    parcel::serialize::BundleToc toc;
    toc.bundle_guid = Guid('3');
    toc.bundle_name = "golden.bundle";
    toc.dependencies.push_back(Guid('4'));
    toc.payloads.push_back(parcel::serialize::BundlePayloadDesc{7U, 0U, 8U, Guid('5'), 0U});
    toc.raw_files.push_back(parcel::serialize::BundleRawFileDesc{"raw/golden.txt", 8U, 8U, 0U});
    const std::vector<std::byte> payload = {
        std::byte{0},  std::byte{1},  std::byte{2},  std::byte{3},  std::byte{4},  std::byte{5},
        std::byte{6},  std::byte{7},  std::byte{8},  std::byte{9},  std::byte{10}, std::byte{11},
        std::byte{12}, std::byte{13}, std::byte{14}, std::byte{15},
    };
    parcel::serialize::BundleWriteOptions options;
    options.file_codec = parcel::serialize::CompressionCodec::None;
    options.toc_codec = parcel::serialize::CompressionCodec::None;
    auto bytes = parcel::serialize::WriteBundle(toc, payload, options);
    if (!bytes) {
        std::abort();
    }
    WriteBytes(path, bytes.Value());
}

void WriteManifestFile(const std::filesystem::path& path) {
    parcel::serialize::ManifestDesc desc;
    desc.package_name = "golden-package";
    desc.package_version = "1.0.0";
    desc.tags.push_back("global");

    parcel::serialize::ManifestBundleDesc bundle;
    bundle.bundle_guid = Guid('6');
    bundle.bundle_name = "golden.bundle";
    bundle.file_name = "golden.bundle";
    bundle.file_size = 16U;
    bundle.file_sha256 = parcel::core::Sha256("golden-bundle");
    bundle.tags.push_back("bundle-tag");
    desc.bundles.push_back(bundle);

    parcel::serialize::ManifestAssetDesc asset;
    asset.asset_guid = Guid('7');
    asset.address = "address/golden";
    asset.asset_path = "Assets/golden.prefab";
    asset.type_name = "parcel.GoldenAsset";
    asset.bundle_id = 0U;
    asset.payload_id = 7U;
    asset.local_object_id = 10U;
    asset.tags.push_back("asset-tag");
    desc.assets.push_back(asset);

    auto bytes = parcel::serialize::WriteManifest(desc);
    if (!bytes) {
        std::abort();
    }
    WriteBytes(path, bytes.Value());
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    WriteAsset(directory / "minimal.prca");
    WriteBundleFile(directory / "minimal.prcl");
    WriteManifestFile(directory / "minimal.prcm");
    return 0;
}
