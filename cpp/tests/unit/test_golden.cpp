#include <catch_amalgamated.hpp>

#include "parcel/core/platform_file.hpp"
#include "parcel/serialize/asset_file.hpp"
#include "parcel/serialize/bundle.hpp"
#include "parcel/serialize/manifest.hpp"

#include <cstddef>
#include <string>
#include <vector>

using parcel::core::PlatformFile;
using parcel::core::ReadAll;
using parcel::serialize::AssetFileReader;
using parcel::serialize::BundleReader;
using parcel::serialize::ManifestView;

namespace {

std::string GoldenPath(const char* name) {
    return std::string(PARCEL_GOLDEN_DIR) + "/" + name;
}

std::vector<std::byte> ReadGolden(const char* name) {
    PlatformFile filesystem;
    auto file = filesystem.OpenRead(GoldenPath(name));
    REQUIRE(file.HasValue());
    auto bytes = ReadAll(*file.Value());
    file.Value()->Close();
    REQUIRE(bytes.HasValue());
    return std::move(bytes).Value();
}

} // namespace

TEST_CASE("golden PRCA file parses", "[serialize][golden]") {
    const auto bytes = ReadGolden("minimal.prca");
    auto reader = AssetFileReader::Open(bytes);
    REQUIRE(reader.HasValue());
    REQUIRE(reader.Value().Types().size() == 1U);
    REQUIRE(reader.Value().Types()[0].type_name == "parcel.GoldenAsset");
    REQUIRE(reader.Value().Objects().size() == 1U);
    REQUIRE(reader.Value().Objects()[0].local_id == 10U);
    REQUIRE(reader.Value().ExternalRefs().size() == 1U);
    REQUIRE(reader.Value().SubAssets().size() == 1U);
    REQUIRE(reader.Value().Blobs().size() == 1U);
    REQUIRE(reader.Value().Blobs()[0].data.size() == 3U);
}

TEST_CASE("golden PRCL file parses", "[serialize][golden]") {
    const auto bytes = ReadGolden("minimal.prcl");
    auto reader = BundleReader::Open(bytes);
    REQUIRE(reader.HasValue());
    REQUIRE(reader.Value().Toc().bundle_name == "golden.bundle");
    REQUIRE(reader.Value().Toc().payloads.size() == 1U);
    REQUIRE(reader.Value().Toc().raw_files.size() == 1U);
    REQUIRE(reader.Value().Header().block_count == 1U);
    auto payload = reader.Value().ReadLogical(0, 16);
    REQUIRE(payload.HasValue());
    REQUIRE(payload.Value().size() == 16U);
    REQUIRE(payload.Value()[15] == std::byte{15});
}

TEST_CASE("golden PRCM file parses", "[serialize][golden]") {
    const auto bytes = ReadGolden("minimal.prcm");
    auto view = ManifestView::Open(bytes);
    REQUIRE(view.HasValue());
    REQUIRE(view.Value().Assets().size() == 1U);
    REQUIRE(view.Value().Bundles().size() == 1U);
    REQUIRE(view.Value().FindAssetByAddress("address/golden") != nullptr);
    REQUIRE(view.Value().FindBundleByFileName("golden.bundle") != nullptr);
}
