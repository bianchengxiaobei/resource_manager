#include <catch_amalgamated.hpp>

#include "parcel/core/guid.hpp"
#include "parcel/serialize/builtin_catalog.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

using parcel::core::AssetGuid;
using parcel::serialize::BuiltinCatalog;
using parcel::serialize::BuiltinCatalogEntry;
using parcel::serialize::DecodeBuiltinCatalog;
using parcel::serialize::EncodeBuiltinCatalog;

namespace {

AssetGuid Guid(char value) {
    std::string text(31, '0');
    text.push_back(value);
    return AssetGuid::FromHex(text).Value();
}

BuiltinCatalog MakeCatalog() {
    BuiltinCatalog catalog;
    catalog.package_name = "game";
    catalog.package_version = "1.0.0";
    catalog.entries.push_back({Guid('a'), "bundle-a.bundle"});
    catalog.entries.push_back({Guid('b'), "bundle-b.bundle"});
    return catalog;
}

} // namespace

TEST_CASE("builtin catalog binary round-trips deterministically") {
    const BuiltinCatalog catalog = MakeCatalog();
    auto encoded = EncodeBuiltinCatalog(catalog);
    REQUIRE(encoded);
    REQUIRE(encoded.Value().size() > 12);
    REQUIRE(static_cast<unsigned char>(encoded.Value()[0]) == 0xEE);
    REQUIRE(static_cast<unsigned char>(encoded.Value()[1]) == 0xC5);
    REQUIRE(static_cast<unsigned char>(encoded.Value()[2]) == 0x33);
    REQUIRE(static_cast<unsigned char>(encoded.Value()[3]) == 0x01);

    auto decoded = DecodeBuiltinCatalog(encoded.Value());
    REQUIRE(decoded);
    REQUIRE(decoded.Value().package_name == catalog.package_name);
    REQUIRE(decoded.Value().package_version == catalog.package_version);
    REQUIRE(decoded.Value().entries.size() == 2);
    REQUIRE(decoded.Value().entries[0].bundle_guid == catalog.entries[0].bundle_guid);
    REQUIRE(decoded.Value().entries[0].file_name == catalog.entries[0].file_name);
    REQUIRE(decoded.Value().entries[1].bundle_guid == catalog.entries[1].bundle_guid);
    REQUIRE(decoded.Value().entries[1].file_name == catalog.entries[1].file_name);

    auto reencoded = EncodeBuiltinCatalog(decoded.Value());
    REQUIRE(reencoded);
    REQUIRE(reencoded.Value() == encoded.Value());
}

TEST_CASE("builtin catalog rejects malformed binary input") {
    const BuiltinCatalog catalog = MakeCatalog();
    auto encoded = EncodeBuiltinCatalog(catalog);
    REQUIRE(encoded);

    auto bad_magic = encoded.Value();
    bad_magic[0] = std::byte{0};
    CHECK_FALSE(DecodeBuiltinCatalog(bad_magic));

    auto bad_version = encoded.Value();
    bad_version[4] = std::byte{2};
    CHECK_FALSE(DecodeBuiltinCatalog(bad_version));

    auto trailing = encoded.Value();
    trailing.push_back(std::byte{0});
    CHECK_FALSE(DecodeBuiltinCatalog(trailing));

    CHECK_FALSE(DecodeBuiltinCatalog(
        std::span<const std::byte>(encoded.Value()).subspan(0, encoded.Value().size() - 1)));
    CHECK_FALSE(DecodeBuiltinCatalog({}));
}

TEST_CASE("builtin catalog encode validates entries and limits") {
    BuiltinCatalog catalog = MakeCatalog();
    catalog.package_name.clear();
    CHECK_FALSE(EncodeBuiltinCatalog(catalog));

    catalog = MakeCatalog();
    catalog.entries[0].bundle_guid = {};
    CHECK_FALSE(EncodeBuiltinCatalog(catalog));

    catalog = MakeCatalog();
    catalog.entries[0].file_name.clear();
    CHECK_FALSE(EncodeBuiltinCatalog(catalog));

    catalog = MakeCatalog();
    catalog.entries[1].file_name = catalog.entries[0].file_name;
    CHECK_FALSE(EncodeBuiltinCatalog(catalog));

    catalog = MakeCatalog();
    catalog.entries[1].bundle_guid = catalog.entries[0].bundle_guid;
    CHECK_FALSE(EncodeBuiltinCatalog(catalog));

    catalog = MakeCatalog();
    catalog.entries[0].file_name = std::string(70000, 'x');
    CHECK_FALSE(EncodeBuiltinCatalog(catalog));
}
