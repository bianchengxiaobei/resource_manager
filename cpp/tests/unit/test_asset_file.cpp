#include <catch_amalgamated.hpp>

#include "parcel/serialize/asset_file.hpp"

#include "test_asset_generated.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using parcel::core::AssetGuid;
using parcel::core::Hash256;
using parcel::core::Sha256;
using parcel::serialize::AssetBlobDesc;
using parcel::serialize::AssetExternalRefDesc;
using parcel::serialize::AssetFileDesc;
using parcel::serialize::AssetFileReader;
using parcel::serialize::AssetObjectDesc;
using parcel::serialize::AssetSubAssetDesc;
using parcel::serialize::AssetTypeDesc;
using parcel::serialize::kAssetHeaderSize;
using parcel::serialize::kAssetMagic;
using parcel::serialize::kNoBlob;
using parcel::serialize::WriteAssetFile;

namespace {

std::vector<std::byte> MakeMetadata(std::string_view name, int value,
                                    std::span<const std::uint8_t> payload) {
    flatbuffers::FlatBufferBuilder builder(256);
    const auto name_offset = builder.CreateString(name.data(), name.size());
    std::vector<std::uint8_t> payload_copy(payload.begin(), payload.end());
    const auto payload_offset = builder.CreateVector(payload_copy);
    const auto root = parcel::test::CreateTestAsset(builder, name_offset, value, payload_offset);
    parcel::test::FinishTestAssetBuffer(builder, root);

    std::vector<std::byte> result(builder.GetSize());
    std::memcpy(result.data(), builder.GetBufferPointer(), builder.GetSize());
    return result;
}

void PatchU32(std::vector<std::byte>& data, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
    }
}

void PatchU64(std::vector<std::byte>& data, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        data[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
    }
}

struct Sample {
    AssetFileDesc desc;
    std::vector<std::byte> metadata_a;
    std::vector<std::byte> metadata_b;
    AssetGuid reference_guid;
};

Sample MakeSample() {
    Sample sample;
    const std::array<std::uint8_t, 4> payload{1U, 2U, 3U, 4U};
    sample.metadata_a = MakeMetadata("first", 11, payload);
    sample.metadata_b = MakeMetadata("second", 22, {});

    sample.desc.types.push_back(AssetTypeDesc{7U, "parcel.test.TestAsset", 3U, 0U});
    sample.desc.objects.push_back(
        AssetObjectDesc{10U, 7U, sample.metadata_a, 0U, 0x1U});
    sample.desc.objects.push_back(
        AssetObjectDesc{11U, 7U, sample.metadata_b, kNoBlob, 0U});
    sample.desc.external_refs.push_back(AssetExternalRefDesc{
        AssetGuid::FromHex("00112233445566778899aabbccddeeff").Value(), 0U});
    sample.desc.sub_assets.push_back(AssetSubAssetDesc{"first-sub", 10U, 0U});
    sample.desc.blobs.push_back(AssetBlobDesc{{std::byte{0xAA}, std::byte{0xBB},
                                               std::byte{0xCC}, std::byte{0xDD}}, 0U});
    sample.reference_guid = sample.desc.external_refs.front().asset_guid;
    return sample;
}

} // namespace

TEST_CASE("PRCA round trip preserves tables and FlatBuffers metadata", "[serialize][asset]") {
    const Sample sample = MakeSample();
    auto bytes = WriteAssetFile(sample.desc);
    REQUIRE(bytes.HasValue());
    REQUIRE(bytes.Value().size() > kAssetHeaderSize);
    REQUIRE(std::memcmp(bytes.Value().data(), kAssetMagic, sizeof(kAssetMagic)) == 0);

    auto reader = AssetFileReader::Open(bytes.Value());
    REQUIRE(reader.HasValue());
    const auto& file = reader.Value();
    REQUIRE(file.Header().type_count == 1U);
    REQUIRE(file.Header().object_count == 2U);
    REQUIRE(file.Header().blob_count == 1U);
    REQUIRE(file.Types().size() == 1U);
    REQUIRE(file.Types()[0].type_name == "parcel.test.TestAsset");
    REQUIRE(file.Objects().size() == 2U);
    REQUIRE(file.Objects()[0].local_id == 10U);
    REQUIRE(file.Objects()[0].metadata.size() == sample.metadata_a.size());
    REQUIRE(std::equal(file.Objects()[0].metadata.begin(), file.Objects()[0].metadata.end(),
                       sample.metadata_a.begin()));
    REQUIRE(file.Objects()[1].blob_id == kNoBlob);
    REQUIRE(file.ExternalRefs().size() == 1U);
    REQUIRE(file.ExternalRefs()[0].asset_guid == sample.reference_guid);
    REQUIRE(file.SubAssets().size() == 1U);
    REQUIRE(file.SubAssets()[0].name == "first-sub");
    REQUIRE(file.Blobs().size() == 1U);
    REQUIRE(file.Blobs()[0].data.size() == 4U);
    REQUIRE(file.Blobs()[0].data[3] == std::byte{0xDD});
    REQUIRE(file.Blobs()[0].crc32 == parcel::core::Crc32(file.Blobs()[0].data));

    flatbuffers::Verifier verifier(
        reinterpret_cast<const std::uint8_t*>(file.Objects()[0].metadata.data()),
        file.Objects()[0].metadata.size());
    REQUIRE(parcel::test::VerifyTestAssetBuffer(verifier));
    const auto* asset = parcel::test::GetTestAsset(file.Objects()[0].metadata.data());
    REQUIRE(asset->name()->str() == "first");
    REQUIRE(asset->value() == 11);
}

TEST_CASE("PRCA supports two-stage header/object reading", "[serialize][asset]") {
    const Sample sample = MakeSample();
    auto bytes = WriteAssetFile(sample.desc);
    REQUIRE(bytes.HasValue());

    AssetFileReader reader(bytes.Value());
    REQUIRE(reader.ReadHeader().HasValue());
    REQUIRE(reader.Objects().empty());
    REQUIRE(reader.ReadObjects().HasValue());
    REQUIRE(reader.Objects().size() == 2U);
    REQUIRE(reader.ReadTypes().HasValue());
    REQUIRE(reader.Types().size() == 1U);
    REQUIRE(reader.ReadBlobs().HasValue());
    REQUIRE(reader.Blobs().size() == 1U);
}

TEST_CASE("PRCA writer rejects invalid descriptions", "[serialize][asset]") {
    Sample sample = MakeSample();

    sample.desc.objects[1].local_id = sample.desc.objects[0].local_id;
    REQUIRE_FALSE(WriteAssetFile(sample.desc).HasValue());

    sample = MakeSample();
    sample.desc.objects[0].type_id = 999U;
    REQUIRE_FALSE(WriteAssetFile(sample.desc).HasValue());

    sample = MakeSample();
    sample.desc.sub_assets[0].local_object_id = 999U;
    REQUIRE_FALSE(WriteAssetFile(sample.desc).HasValue());

    sample = MakeSample();
    sample.desc.external_refs[0].asset_guid = AssetGuid{};
    REQUIRE_FALSE(WriteAssetFile(sample.desc).HasValue());

    sample = MakeSample();
    sample.desc.objects[0].blob_id = 99U;
    REQUIRE_FALSE(WriteAssetFile(sample.desc).HasValue());

    sample = MakeSample();
    sample.desc.types[0].type_name.clear();
    REQUIRE_FALSE(WriteAssetFile(sample.desc).HasValue());
}

TEST_CASE("PRCA reader rejects corruption and truncation", "[serialize][asset]") {
    const Sample sample = MakeSample();
    auto bytes = WriteAssetFile(sample.desc);
    REQUIRE(bytes.HasValue());

    auto bad_magic = bytes.Value();
    bad_magic[0] = std::byte{'X'};
    REQUIRE_FALSE(AssetFileReader::Open(bad_magic, false).HasValue());

    auto truncated = bytes.Value();
    truncated.resize(kAssetHeaderSize - 1);
    REQUIRE_FALSE(AssetFileReader::Open(truncated, false).HasValue());

    auto bad_hash = bytes.Value();
    bad_hash[92] ^= std::byte{0xFF};
    REQUIRE_FALSE(AssetFileReader::Open(bad_hash, true).HasValue());

    auto bad_version = bytes.Value();
    bad_version[4] = std::byte{99};
    bad_version[5] = std::byte{0};
    REQUIRE_FALSE(AssetFileReader::Open(bad_version, false).HasValue());

    auto bad_blob = bytes.Value();
    {
        auto valid = AssetFileReader::Open(bad_blob, false);
        REQUIRE(valid.HasValue());
        const auto blob = valid.Value().Blobs()[0].data;
        const std::size_t offset =
            static_cast<std::size_t>(blob.data() - bad_blob.data());
        bad_blob[offset + 1] ^= std::byte{0x11};
    }
    REQUIRE_FALSE(AssetFileReader::Open(bad_blob, false).HasValue());

    auto bad_metadata = bytes.Value();
    {
        auto valid = AssetFileReader::Open(bad_metadata, false);
        REQUIRE(valid.HasValue());
        const std::uint64_t object_table = valid.Value().Header().object_table_offset;
        PatchU64(bad_metadata, static_cast<std::size_t>(object_table) + 8U,
                 static_cast<std::uint64_t>(bad_metadata.size()) + 4096U);
    }
    REQUIRE_FALSE(AssetFileReader::Open(bad_metadata, false).HasValue());
}

TEST_CASE("PRCA reader validates type references and blob overlap", "[serialize][asset]") {
    const Sample sample = MakeSample();
    auto bytes = WriteAssetFile(sample.desc);
    REQUIRE(bytes.HasValue());

    auto bad_type = bytes.Value();
    {
        auto valid = AssetFileReader::Open(bad_type, false);
        REQUIRE(valid.HasValue());
        const std::uint64_t table = valid.Value().Header().object_table_offset;
        PatchU32(bad_type, static_cast<std::size_t>(table) + 4U, 999U);
    }
    REQUIRE_FALSE(AssetFileReader::Open(bad_type, false).HasValue());

    AssetFileDesc two_blobs = sample.desc;
    two_blobs.blobs.push_back(AssetBlobDesc{{std::byte{0xEE}}, 0U});
    auto blob_bytes = WriteAssetFile(two_blobs);
    REQUIRE(blob_bytes.HasValue());
    auto overlapping = blob_bytes.Value();
    {
        auto valid = AssetFileReader::Open(overlapping, false);
        REQUIRE(valid.HasValue());
        const std::uint64_t table = valid.Value().Header().blob_table_offset;
        const std::uint64_t first_offset =
            static_cast<const std::byte*>(valid.Value().Blobs()[0].data.data()) -
            static_cast<const std::byte*>(overlapping.data());
        PatchU64(overlapping, static_cast<std::size_t>(table) + 24U, first_offset);
    }
    REQUIRE_FALSE(AssetFileReader::Open(overlapping, false).HasValue());
}
