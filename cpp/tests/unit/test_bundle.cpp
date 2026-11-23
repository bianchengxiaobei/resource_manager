#include <catch_amalgamated.hpp>

#include "parcel/serialize/binary.hpp"
#include "parcel/serialize/bundle.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using parcel::core::AssetGuid;
using parcel::serialize::BinaryReader;
using parcel::serialize::BundlePayloadDesc;
using parcel::serialize::BundleRawFileDesc;
using parcel::serialize::BundleReader;
using parcel::serialize::BundleToc;
using parcel::serialize::BundleWriteOptions;
using parcel::serialize::CompressionCodec;
using parcel::serialize::kBundleMagic;
using parcel::serialize::WriteBundle;

namespace {

std::vector<std::byte> MakePayload(std::size_t size) {
    std::vector<std::byte> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>((i * 73U + (i >> 7U)) & 0xFFU);
    }
    return data;
}

AssetGuid Guid(char suffix) {
    std::string text = "00112233445566778899aabbccddee0";
    text.push_back(suffix);
    return AssetGuid::FromHex(text).Value();
}

BundleToc MakeToc() {
    BundleToc toc;
    toc.bundle_guid = Guid('1');
    toc.bundle_name = "level01.bundle";
    toc.dependencies.push_back(Guid('2'));
    toc.payloads.push_back(
        BundlePayloadDesc{7U, 0U, 1024U, Guid('3'), 0U});
    toc.payloads.push_back(
        BundlePayloadDesc{8U, 70000U, 5000U, Guid('4'), 0U});
    toc.raw_files.push_back(BundleRawFileDesc{"raw/readme.txt", 200000U, 1000U, 0U});
    return toc;
}

void PatchU64(std::vector<std::byte>& data, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        data[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
    }
}

} // namespace

TEST_CASE("PRCL round trip with each codec combination", "[serialize][bundle]") {
    const std::vector<std::byte> payload = MakePayload(300000);
    const BundleToc toc = MakeToc();

    for (const CompressionCodec file_codec :
         {CompressionCodec::None, CompressionCodec::Lz4, CompressionCodec::Zstd}) {
        for (const CompressionCodec toc_codec :
             {CompressionCodec::None, CompressionCodec::Lz4, CompressionCodec::Zstd}) {
            INFO("file_codec=" << parcel::serialize::ToString(file_codec)
                               << " toc_codec=" << parcel::serialize::ToString(toc_codec));
            BundleWriteOptions options;
            options.block_size = 64U * 1024U;
            options.file_codec = file_codec;
            options.toc_codec = toc_codec;
            auto bytes = WriteBundle(toc, payload, options);
            REQUIRE(bytes.HasValue());
            REQUIRE(bytes.Value().size() > 128);
            REQUIRE(std::memcmp(bytes.Value().data(), kBundleMagic, 4) == 0);

            auto reader = BundleReader::Open(bytes.Value());
            REQUIRE(reader.HasValue());
            auto& bundle = reader.Value();
            REQUIRE(bundle.Header().block_count == 5U);
            REQUIRE(bundle.Header().block_size == 64U * 1024U);
            REQUIRE(bundle.LogicalSize() == payload.size());
            REQUIRE(bundle.Toc().bundle_name == toc.bundle_name);
            REQUIRE(bundle.Toc().dependencies.size() == 1U);
            REQUIRE(bundle.Toc().payloads.size() == 2U);
            REQUIRE(bundle.Toc().raw_files.size() == 1U);
            REQUIRE(bundle.Toc().raw_files[0].name == "raw/readme.txt");

            auto all = bundle.ReadAllPayload();
            REQUIRE(all.HasValue());
            REQUIRE(all.Value() == payload);

            auto span = bundle.ReadLogical(60U * 1024U, 10U * 1024U);
            REQUIRE(span.HasValue());
            REQUIRE(span.Value().size() == 10U * 1024U);
            REQUIRE(std::equal(span.Value().begin(), span.Value().end(),
                               payload.begin() + 60U * 1024U));
        }
    }
}

TEST_CASE("PRCL supports empty logical payload", "[serialize][bundle]") {
    BundleToc toc = MakeToc();
    toc.payloads.clear();
    toc.raw_files.clear();
    auto bytes = WriteBundle(toc, {});
    REQUIRE(bytes.HasValue());

    auto reader = BundleReader::Open(bytes.Value());
    REQUIRE(reader.HasValue());
    REQUIRE(reader.Value().Header().block_count == 0U);
    REQUIRE(reader.Value().LogicalSize() == 0U);
    auto all = reader.Value().ReadAllPayload();
    REQUIRE(all.HasValue());
    REQUIRE(all.Value().empty());
}

TEST_CASE("PRCL reads only blocks covering the requested range", "[serialize][bundle]") {
    const std::vector<std::byte> payload = MakePayload(300000);
    BundleWriteOptions options;
    options.block_size = 64U * 1024U;
    options.file_codec = CompressionCodec::Lz4;
    auto bytes = WriteBundle(MakeToc(), payload, options);
    REQUIRE(bytes.HasValue());

    BundleReader reader(bytes.Value());
    REQUIRE(reader.ReadHeader().HasValue());
    REQUIRE(reader.ReadBlocks().HasValue());

    const auto& header = reader.Header();
    REQUIRE(header.block_count == 5U);
    const std::size_t block_entry =
        static_cast<std::size_t>(header.block_table_offset) + 3U * 24U;
    BinaryReader block_reader(std::span<const std::byte>(bytes.Value()).subspan(
        block_entry, 24));
    const std::uint64_t data_offset = block_reader.ReadU64().Value();
    bytes.Value().at(static_cast<std::size_t>(data_offset)) ^= std::byte{0x5A};

    auto early = reader.ReadLogical(0, 16);
    REQUIRE(early.HasValue());
    REQUIRE(std::equal(early.Value().begin(), early.Value().end(), payload.begin()));

    auto damaged = reader.ReadLogical(3U * 64U * 1024U, 16);
    REQUIRE_FALSE(damaged.HasValue());
}

TEST_CASE("PRCL rejects invalid TOC contents", "[serialize][bundle]") {
    const std::vector<std::byte> payload = MakePayload(300000);

    BundleToc toc = MakeToc();
    toc.raw_files[0].name = "../escape.txt";
    REQUIRE_FALSE(WriteBundle(toc, payload).HasValue());

    toc = MakeToc();
    toc.payloads[1].payload_id = toc.payloads[0].payload_id;
    REQUIRE_FALSE(WriteBundle(toc, payload).HasValue());

    toc = MakeToc();
    toc.payloads[0].size = 200000U;
    REQUIRE_FALSE(WriteBundle(toc, payload).HasValue());

    toc = MakeToc();
    toc.dependencies.push_back(toc.dependencies[0]);
    REQUIRE_FALSE(WriteBundle(toc, payload).HasValue());

    toc = MakeToc();
    toc.bundle_guid = AssetGuid{};
    REQUIRE_FALSE(WriteBundle(toc, payload).HasValue());

    toc = MakeToc();
    toc.raw_files.push_back(BundleRawFileDesc{"raw/readme.txt", 0U, 10U, 0U});
    REQUIRE_FALSE(WriteBundle(toc, payload).HasValue());
}

TEST_CASE("PRCL detects corruption", "[serialize][bundle]") {
    const std::vector<std::byte> payload = MakePayload(300000);
    auto bytes = WriteBundle(MakeToc(), payload);
    REQUIRE(bytes.HasValue());

    auto bad_magic = bytes.Value();
    bad_magic[0] = std::byte{'X'};
    REQUIRE_FALSE(BundleReader::Open(bad_magic).HasValue());

    auto bad_header = bytes.Value();
    bad_header[8] ^= std::byte{0xFF};
    REQUIRE_FALSE(BundleReader::Open(bad_header).HasValue());

    auto bad_version = bytes.Value();
    bad_version[4] = std::byte{99};
    bad_version[5] = std::byte{0};
    REQUIRE_FALSE(BundleReader::Open(bad_version, false).HasValue());

    auto bad_file_hash = bytes.Value();
    bad_file_hash.back() ^= std::byte{0xFF};
    REQUIRE_FALSE(BundleReader::Open(bad_file_hash, true).HasValue());

    auto bad_block = bytes.Value();
    {
        auto valid = BundleReader::Open(bad_block, false);
        REQUIRE(valid.HasValue());
        const std::uint64_t table = valid.Value().Header().block_table_offset;
        PatchU64(bad_block, static_cast<std::size_t>(table),
                 static_cast<std::uint64_t>(bad_block.size()) + 4096U);
    }
    REQUIRE_FALSE(BundleReader::Open(bad_block, false).HasValue());

    auto truncated = bytes.Value();
    truncated.resize(100);
    REQUIRE_FALSE(BundleReader::Open(truncated).HasValue());
}
