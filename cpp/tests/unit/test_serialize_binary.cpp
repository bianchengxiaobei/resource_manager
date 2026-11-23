#include <catch_amalgamated.hpp>

#include "parcel/serialize/binary.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

using parcel::serialize::BinaryReader;
using parcel::serialize::BinaryWriter;

TEST_CASE("binary writer and reader round trip scalars", "[serialize][binary]") {
    BinaryWriter writer;
    REQUIRE(writer.WriteU8(0x12U).HasValue());
    REQUIRE(writer.WriteU16(0x3456U).HasValue());
    REQUIRE(writer.WriteU32(0x789ABCDEU).HasValue());
    REQUIRE(writer.WriteU64(0x0123456789ABCDEFULL).HasValue());
    REQUIRE(writer.WriteI32(-123456).HasValue());
    REQUIRE(writer.WriteI64(-9876543210LL).HasValue());
    REQUIRE(writer.WriteF32(1.5F).HasValue());
    REQUIRE(writer.WriteF64(-2.25).HasValue());
    REQUIRE(writer.WriteBool(true).HasValue());
    REQUIRE(writer.WriteBool(false).HasValue());
    REQUIRE(writer.WriteString("parcel").HasValue());
    REQUIRE(writer.WriteCString("asset-type").HasValue());

    const std::vector<std::byte> data = writer.Release();
    BinaryReader reader(data);
    REQUIRE(reader.ReadU8().Value() == 0x12U);
    REQUIRE(reader.ReadU16().Value() == 0x3456U);
    REQUIRE(reader.ReadU32().Value() == 0x789ABCDEU);
    REQUIRE(reader.ReadU64().Value() == 0x0123456789ABCDEFULL);
    REQUIRE(reader.ReadI32().Value() == -123456);
    REQUIRE(reader.ReadI64().Value() == -9876543210LL);
    REQUIRE(reader.ReadF32().Value() == 1.5F);
    REQUIRE(reader.ReadF64().Value() == -2.25);
    REQUIRE(reader.ReadBool().Value());
    REQUIRE_FALSE(reader.ReadBool().Value());
    REQUIRE(reader.ReadString().Value() == "parcel");
    REQUIRE(reader.ReadCString().Value() == "asset-type");
    REQUIRE(reader.IsAtEnd());
}

TEST_CASE("binary writer patch and alignment", "[serialize][binary]") {
    BinaryWriter writer;
    REQUIRE(writer.WriteU32(0U).HasValue());
    REQUIRE(writer.Align(8).HasValue());
    REQUIRE(writer.Tell() == 8);
    REQUIRE(writer.WriteU64(0U).HasValue());
    REQUIRE(writer.PatchU32(0, 0xDEADBEEFU).HasValue());
    REQUIRE(writer.PatchU64(8, 0x1122334455667788ULL).HasValue());
    REQUIRE_FALSE(writer.PatchU32(15, 1U).HasValue());
    REQUIRE_FALSE(writer.Align(0).HasValue());

    BinaryReader reader(writer.Data());
    REQUIRE(reader.ReadU32().Value() == 0xDEADBEEFU);
    REQUIRE(reader.Skip(4).HasValue());
    REQUIRE(reader.ReadU64().Value() == 0x1122334455667788ULL);
}

TEST_CASE("binary reader rejects truncated and malformed input", "[serialize][binary]") {
    const std::array<std::byte, 2> tiny{std::byte{0}, std::byte{0}};
    BinaryReader tiny_reader(tiny);
    REQUIRE_FALSE(tiny_reader.ReadU32().HasValue());
    REQUIRE(tiny_reader.Position() == 0);

    const std::array<std::byte, 1> invalid_bool{std::byte{2}};
    BinaryReader bool_reader(invalid_bool);
    REQUIRE_FALSE(bool_reader.ReadBool().HasValue());

    const std::array<std::byte, 5> bad_string{
        std::byte{5}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{'a'}};
    BinaryReader string_reader(bad_string);
    REQUIRE_FALSE(string_reader.ReadString().HasValue());

    const std::array<std::byte, 3> unterminated{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    BinaryReader cstring_reader(unterminated);
    REQUIRE_FALSE(cstring_reader.ReadCString().HasValue());

    BinaryReader reader(bad_string);
    const auto bytes = reader.ReadBytes(2);
    REQUIRE(bytes.HasValue());
    REQUIRE(bytes.Value().size() == 2);
    REQUIRE_FALSE(reader.ReadBytes(99).HasValue());
}

TEST_CASE("binary hash and GUID round trip", "[serialize][binary]") {
    const auto hash = parcel::core::Sha256("parcel");
    const auto guid = parcel::core::Guid128::FromHex("00112233445566778899aabbccddeeff").Value();

    BinaryWriter writer;
    REQUIRE(writer.WriteHash256(hash).HasValue());
    REQUIRE(writer.WriteGuid128(guid).HasValue());

    BinaryReader reader(writer.Data());
    REQUIRE(reader.ReadHash256().Value() == hash);
    REQUIRE(reader.ReadGuid128().Value() == guid);
    REQUIRE(reader.IsAtEnd());
}
