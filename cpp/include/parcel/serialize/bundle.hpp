#pragma once

#include "parcel/core/guid.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/result.hpp"
#include "parcel/serialize/compression.hpp"
#include "parcel/serialize/format.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::serialize {

using core::Result;

inline constexpr std::uint32_t kBundleFlagCompressedToc = 1U << 0U;
inline constexpr std::size_t kBundleFileHashOffset = 60;
inline constexpr std::size_t kBundleHeaderCrcOffset = 92;

struct BundleHeader {
    std::uint16_t version = kBundleVersion;
    std::uint16_t header_size = kBundleHeaderSize;
    std::uint32_t flags = 0;
    std::uint64_t file_size = 0;
    std::uint64_t block_table_offset = 0;
    std::uint32_t block_table_size = 0;
    std::uint32_t block_count = 0;
    std::uint64_t toc_offset = 0;
    std::uint32_t toc_compressed_size = 0;
    std::uint32_t toc_raw_size = 0;
    std::uint32_t block_size = kDefaultBlockSize;
    CompressionCodec toc_codec = CompressionCodec::None;
    CompressionCodec file_codec = CompressionCodec::None;
    core::Hash256 file_hash;
    std::uint32_t header_crc = 0;
};

struct BundleBlock {
    std::uint64_t data_offset = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t crc32 = 0;
    CompressionCodec codec = CompressionCodec::None;
    std::uint16_t flags = 0;
};

struct BundlePayloadDesc {
    std::uint32_t payload_id = 0;
    std::uint64_t logical_offset = 0;
    std::uint64_t size = 0;
    core::AssetGuid main_asset_guid;
    std::uint32_t flags = 0;
};

struct BundleRawFileDesc {
    std::string name;
    std::uint64_t logical_offset = 0;
    std::uint64_t size = 0;
    std::uint32_t flags = 0;
};

struct BundleToc {
    core::AssetGuid bundle_guid;
    std::string bundle_name;
    std::vector<core::AssetGuid> dependencies;
    std::vector<BundlePayloadDesc> payloads;
    std::vector<BundleRawFileDesc> raw_files;
};

struct BundleWriteOptions {
    std::uint32_t block_size = kDefaultBlockSize;
    CompressionCodec file_codec = CompressionCodec::None;
    CompressionCodec toc_codec = CompressionCodec::Lz4;
    int compression_level = 3;
};

[[nodiscard]] Result<std::vector<std::byte>> WriteBundle(
    const BundleToc& toc, std::span<const std::byte> logical_payload,
    const BundleWriteOptions& options = {});

class BundleWriter {
  public:
    explicit BundleWriter(BundleToc toc, BundleWriteOptions options = {});

    [[nodiscard]] Result<std::vector<std::byte>> Build(
        std::span<const std::byte> logical_payload) const;

  private:
    BundleToc toc_;
    BundleWriteOptions options_;
};

class BundleReader {
  public:
    explicit BundleReader(std::span<const std::byte> data) noexcept;

    static Result<BundleReader> Open(std::span<const std::byte> data,
                                     bool verify_file_hash = true);

    [[nodiscard]] Result<void> ReadHeader();
    [[nodiscard]] Result<void> ReadBlocks();
    [[nodiscard]] Result<void> ReadToc();
    [[nodiscard]] Result<void> ReadAll(bool verify_file_hash = true);
    [[nodiscard]] Result<void> VerifyHeaderCrc() const;
    [[nodiscard]] Result<void> VerifyFileHash() const;

    [[nodiscard]] Result<std::vector<std::byte>> ReadLogical(std::uint64_t offset,
                                                             std::uint64_t size);
    [[nodiscard]] Result<std::vector<std::byte>> ReadAllPayload();

    [[nodiscard]] const BundleHeader& Header() const noexcept { return header_; }
    [[nodiscard]] std::span<const BundleBlock> Blocks() const noexcept { return blocks_; }
    [[nodiscard]] const BundleToc& Toc() const noexcept { return toc_; }
    [[nodiscard]] std::uint64_t LogicalSize() const noexcept { return logical_size_; }

  private:
    [[nodiscard]] Result<const std::vector<std::byte>*> LoadBlock(std::uint32_t index);

    std::span<const std::byte> data_;
    BundleHeader header_;
    bool header_read_ = false;
    std::vector<BundleBlock> blocks_;
    std::uint64_t logical_size_ = 0;
    BundleToc toc_;
    std::vector<std::byte> toc_raw_;
    std::vector<std::byte> cached_block_;
    std::uint32_t cached_block_index_ = static_cast<std::uint32_t>(-1);
};

} // namespace parcel::serialize
