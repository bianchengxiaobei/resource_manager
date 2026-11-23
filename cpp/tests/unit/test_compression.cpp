#include <catch_amalgamated.hpp>

#include "parcel/serialize/compression.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using parcel::serialize::CompressionCodec;
using parcel::serialize::Compress;
using parcel::serialize::Decompress;
using parcel::serialize::ParseCompressionCodec;

namespace {

std::vector<std::byte> MakePattern(std::size_t size) {
    std::vector<std::byte> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>((i * 37U + (i >> 8U)) & 0xFFU);
    }
    return data;
}

} // namespace

TEST_CASE("compression round trips supported codecs", "[serialize][compression]") {
    const std::vector<std::byte> input = MakePattern(1024U * 1024U);

    for (const CompressionCodec codec :
         {CompressionCodec::None, CompressionCodec::Lz4, CompressionCodec::Zstd}) {
        INFO("codec: " << parcel::serialize::ToString(codec));
        auto compressed = Compress(codec, input);
        REQUIRE(compressed.HasValue());
        auto decompressed = Decompress(codec, compressed.Value(), input.size());
        REQUIRE(decompressed.HasValue());
        REQUIRE(decompressed.Value() == input);
    }
}

TEST_CASE("compression handles empty input", "[serialize][compression]") {
    for (const CompressionCodec codec :
         {CompressionCodec::None, CompressionCodec::Lz4, CompressionCodec::Zstd}) {
        auto compressed = Compress(codec, {});
        REQUIRE(compressed.HasValue());
        REQUIRE(compressed.Value().empty());
        auto decompressed = Decompress(codec, {}, 0);
        REQUIRE(decompressed.HasValue());
        REQUIRE(decompressed.Value().empty());
    }
}

TEST_CASE("compression rejects unknown codecs and bad sizes", "[serialize][compression]") {
    REQUIRE_FALSE(ParseCompressionCodec(200).HasValue());

    const std::vector<std::byte> input = MakePattern(4096);
    auto compressed = Compress(CompressionCodec::Lz4, input);
    REQUIRE(compressed.HasValue());
    REQUIRE_FALSE(Decompress(CompressionCodec::Lz4, compressed.Value(), input.size() + 1).HasValue());
    REQUIRE_FALSE(
        Decompress(CompressionCodec::None, compressed.Value(), input.size() + 1).HasValue());

    auto zstd = Compress(CompressionCodec::Zstd, input);
    REQUIRE(zstd.HasValue());
    zstd.Value().resize(zstd.Value().size() / 2);
    REQUIRE_FALSE(Decompress(CompressionCodec::Zstd, zstd.Value(), input.size()).HasValue());
}

TEST_CASE("compression detects corrupted payloads", "[serialize][compression]") {
    const std::vector<std::byte> input = MakePattern(8192);
    for (const CompressionCodec codec : {CompressionCodec::Lz4, CompressionCodec::Zstd}) {
        auto compressed = Compress(codec, input);
        REQUIRE(compressed.HasValue());
        compressed.Value().at(0) ^= std::byte{0x5A};
        auto decompressed = Decompress(codec, compressed.Value(), input.size());
        REQUIRE_FALSE(decompressed.HasValue());
    }
}
