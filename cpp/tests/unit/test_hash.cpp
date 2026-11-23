#include <catch_amalgamated.hpp>

#include "parcel/core/hash.hpp"
#include "parcel/core/platform_file.hpp"
#include "test_util.hpp"

#include <array>
#include <span>
#include <string>

using parcel::core::Crc32;
using parcel::core::Hash256;
using parcel::core::PlatformFile;
using parcel::core::Sha256;
using parcel::core::Sha256File;
using parcel::core::WriteAll;

TEST_CASE("SHA-256 known vectors", "[core][hash]") {
    REQUIRE(Sha256("").ToHex() ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(Sha256("abc").ToHex() ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(Sha256("The quick brown fox jumps over the lazy dog").ToHex() ==
            "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST_CASE("SHA-256 hex round trip and validation", "[core][hash]") {
    const auto digest = Sha256("parcel");
    const auto parsed = Hash256::FromHex(digest.ToHex());
    REQUIRE(parsed.HasValue());
    REQUIRE(parsed.Value() == digest);

    REQUIRE_FALSE(Hash256::FromHex("not-a-hash").HasValue());
    REQUIRE_FALSE(
        Hash256::FromHex("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz")
            .HasValue());
}

TEST_CASE("CRC32 known vector", "[core][hash]") {
    REQUIRE(Crc32("123456789") == 0xcbf43926U);
    REQUIRE(Crc32("") == 0U);
}

TEST_CASE("File hashing matches memory hashing", "[core][hash]") {
    parcel::test::TempDir temp("hash");
    const auto path = temp.Path() / "payload.bin";
    const std::string payload = "Parcel file hash test";

    PlatformFile filesystem;
    auto file_result = filesystem.OpenWrite(parcel::test::ToUtf8(path));
    REQUIRE(file_result.HasValue());
    auto& file = *file_result.Value();
    REQUIRE(WriteAll(file, std::as_bytes(std::span(payload.data(), payload.size()))).HasValue());
    REQUIRE(file.Flush().HasValue());
    file.Close();

    const auto file_hash = Sha256File(parcel::test::ToUtf8(path));
    REQUIRE(file_hash.HasValue());
    REQUIRE(file_hash.Value() == Sha256(payload));
}
