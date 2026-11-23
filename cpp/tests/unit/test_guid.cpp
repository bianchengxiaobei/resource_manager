#include <catch_amalgamated.hpp>

#include "parcel/core/guid.hpp"

#include <array>
#include <cstddef>
#include <unordered_map>

using parcel::core::Guid128;
using parcel::core::Guid128Hash;

TEST_CASE("Guid128 zero and hex conversion", "[core][guid]") {
    const Guid128 zero;
    REQUIRE(zero.IsZero());
    REQUIRE(zero.ToHex() == "00000000000000000000000000000000");

    const auto parsed = Guid128::FromHex("00112233-4455-6677-8899-aabbccddeeff");
    REQUIRE(parsed.HasValue());
    REQUIRE_FALSE(parsed.Value().IsZero());
    REQUIRE(parsed.Value().ToHex() == "00112233445566778899aabbccddeeff");
    REQUIRE(parsed.Value().bytes[0] == 0x00U);
    REQUIRE(parsed.Value().bytes[1] == 0x11U);
    REQUIRE(parsed.Value().bytes[15] == 0xFFU);

    REQUIRE(Guid128::FromHex("00112233445566778899AABBCCDDEEFF").HasValue());
    REQUIRE_FALSE(Guid128::FromHex("0011").HasValue());
    REQUIRE_FALSE(Guid128::FromHex("00112233445566778899aabbccddeefz").HasValue());
}

TEST_CASE("Guid128 ordering and hashing", "[core][guid]") {
    const auto first = Guid128::FromHex("00000000000000000000000000000001").Value();
    const auto second = Guid128::FromHex("00000000000000000000000000000002").Value();

    REQUIRE(first < second);
    REQUIRE(Guid128Hash{}(first) == Guid128Hash{}(first));
    REQUIRE(Guid128Hash{}(first) != Guid128Hash{}(second));

    std::unordered_map<Guid128, int, Guid128Hash> values;
    values.emplace(first, 1);
    values.emplace(second, 2);
    REQUIRE(values.at(first) == 1);
    REQUIRE(values.at(second) == 2);
}

TEST_CASE("Guid128 converts from bytes", "[core][guid]") {
    std::array<std::byte, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(i + 1);
    }
    const Guid128 guid = Guid128::FromBytes(bytes);
    REQUIRE(guid.ToHex() == "0102030405060708090a0b0c0d0e0f10");
}
