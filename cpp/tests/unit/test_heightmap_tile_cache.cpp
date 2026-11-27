#include <catch_amalgamated.hpp>

#include "parcel/hbg/heightmap_tile_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

void WriteU16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffu);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void WriteU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xffu);
    }
}

void WriteF32(std::vector<std::byte>& bytes, std::size_t offset, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32(bytes, offset, bits);
}

std::vector<std::byte> RampHeightmap() {
    constexpr std::uint32_t width = 5;
    constexpr std::uint32_t height = 5;
    std::vector<std::byte> bytes(20 + width * height * sizeof(std::uint16_t));
    std::memcpy(bytes.data(), "HBGHMAP1", 8);
    WriteU32(bytes, 8, width);
    WriteU32(bytes, 12, height);
    WriteF32(bytes, 16, 1.0f);
    for (std::uint32_t z = 0; z < height; ++z) {
        for (std::uint32_t x = 0; x < width; ++x) {
            WriteU16(bytes, 20 + (z * width + x) * sizeof(std::uint16_t),
                     static_cast<std::uint16_t>(x * 10));
        }
    }
    return bytes;
}

} // namespace

TEST_CASE("heightmap tile cache samples bilinear heights") {
    const std::vector<std::byte> bytes = RampHeightmap();
    parcel_hbg::HeightmapTileCache cache;
    std::string error;
    REQUIRE(cache.Load(bytes, &error));
    CHECK(error.empty());
    CHECK(cache.loaded());
    CHECK(cache.width() == 5);
    CHECK(cache.height() == 5);
    CHECK(cache.height_scale() == 1.0f);

    cache.SetWorld(-50.0f, -50.0f, 100.0f, 2.0f);
    cache.SetCapacity(8);

    float height = 0.0f;
    REQUIRE(cache.SampleHeight(-50.0f, -50.0f, height));
    CHECK(height == Catch::Approx(0.0f));
    REQUIRE(cache.SampleHeight(50.0f, 50.0f, height));
    CHECK(height == Catch::Approx(40.0f));
    REQUIRE(cache.SampleHeight(0.0f, 0.0f, height));
    CHECK(height == Catch::Approx(20.0f));
    REQUIRE(cache.SampleHeight(25.0f, 0.0f, height));
    CHECK(height == Catch::Approx(30.0f));
    CHECK_FALSE(cache.SampleHeight(60.0f, 0.0f, height));

    float zmin = 0.0f;
    float zmax = 0.0f;
    REQUIRE(cache.Query(0, 0, 0, zmin, zmax));
    CHECK(zmin == Catch::Approx(-2.0f));
    CHECK(zmax == Catch::Approx(40.0f));
    REQUIRE(cache.Query(1, 0, 0, zmin, zmax));
    CHECK(zmax == Catch::Approx(30.0f));
    REQUIRE(cache.Query(1, 1, 0, zmin, zmax));
    CHECK(zmin == Catch::Approx(8.0f));
    CHECK(zmax == Catch::Approx(40.0f));

    const parcel_hbg::HeightmapTileCache::Stats stats = cache.stats();
    CHECK(stats.loaded_tiles == 3);
    CHECK(stats.misses == 3);
    REQUIRE(cache.Query(1, 1, 0, zmin, zmax));
    CHECK(cache.stats().hits == 1);

    parcel_hbg::HeightmapTileCache small;
    REQUIRE(small.Load(bytes, &error));
    small.SetWorld(-50.0f, -50.0f, 100.0f, 2.0f);
    small.SetCapacity(1);
    REQUIRE(small.Query(0, 0, 0, zmin, zmax));
    REQUIRE(small.Query(1, 1, 0, zmin, zmax));
    CHECK(small.stats().evictions == 1);
    REQUIRE(small.Query(0, 0, 0, zmin, zmax));
    CHECK(small.stats().misses == 2);
    CHECK(small.stats().hits == 1);
}

TEST_CASE("heightmap tile cache rejects malformed data") {
    parcel_hbg::HeightmapTileCache cache;
    std::string error;

    CHECK_FALSE(cache.Load(std::vector<std::byte>{}, &error));
    CHECK_FALSE(error.empty());

    std::vector<std::byte> bytes = RampHeightmap();
    bytes[0] = std::byte{0};
    CHECK_FALSE(cache.Load(bytes, &error));

    bytes = RampHeightmap();
    bytes.resize(21);
    CHECK_FALSE(cache.Load(bytes, &error));
    CHECK_FALSE(cache.loaded());
}
