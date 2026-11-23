#include "parcel/serialize/asset_file.hpp"
#include "parcel/serialize/bundle.hpp"
#include "parcel/serialize/manifest.hpp"

#if defined(PARCEL_FUZZ_FORMAT_SIDECAR)
#include "parcel/assets/sidecar.hpp"
#include "parcel/import/meta.hpp"
#include "parcel/remote/remote.hpp"
#include <string_view>
#endif

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
#if defined(PARCEL_FUZZ_FORMAT_SIDECAR)
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    (void)parcel::assets::ParseSceneSidecar(text);
    (void)parcel::import::ParseAssetMeta(text);
    (void)parcel::remote::ParseRemoteConfig(text);
#elif defined(PARCEL_FUZZ_FORMAT_ASSET)
    (void)parcel::serialize::AssetFileReader::Open(bytes, false);
#elif defined(PARCEL_FUZZ_FORMAT_BUNDLE)
    (void)parcel::serialize::BundleReader::Open(bytes, false);
#elif defined(PARCEL_FUZZ_FORMAT_MANIFEST)
    (void)parcel::serialize::ManifestView::Open(bytes, false);
#else
    (void)parcel::serialize::AssetFileReader::Open(bytes, false);
    (void)parcel::serialize::BundleReader::Open(bytes, false);
    (void)parcel::serialize::ManifestView::Open(bytes, false);
#endif
    return 0;
}

#if defined(PARCEL_FUZZ_STANDALONE)
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }
    std::ifstream stream(argv[1], std::ios::binary);
    if (!stream) {
        return 1;
    }
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(stream)),
                                         std::istreambuf_iterator<char>());
    return LLVMFuzzerTestOneInput(data.data(), data.size());
}
#endif



