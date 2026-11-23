#include "parcel/serialize/bundle.hpp"

#include "parcel/core/hash.hpp"
#include "parcel/core/path.hpp"
#include "parcel/serialize/binary.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <unordered_set>

namespace parcel::serialize {
namespace {

constexpr std::size_t kBlockEntrySize = 24;

constexpr std::size_t kHeaderMagic = 0;
constexpr std::size_t kHeaderVersion = 4;
constexpr std::size_t kHeaderSize = 6;
constexpr std::size_t kHeaderFlags = 8;
constexpr std::size_t kHeaderFileSize = 12;
constexpr std::size_t kHeaderBlockTableOffset = 20;
constexpr std::size_t kHeaderBlockTableSize = 28;
constexpr std::size_t kHeaderBlockCount = 32;
constexpr std::size_t kHeaderTocOffset = 36;
constexpr std::size_t kHeaderTocCompressedSize = 44;
constexpr std::size_t kHeaderTocRawSize = 48;
constexpr std::size_t kHeaderBlockSize = 52;
constexpr std::size_t kHeaderTocCodec = 56;
constexpr std::size_t kHeaderFileCodec = 57;
constexpr std::size_t kHeaderFileHash = 60;
constexpr std::size_t kHeaderHeaderCrc = 92;

constexpr std::size_t kBlockDataOffset = 0;
constexpr std::size_t kBlockCompressedSize = 8;
constexpr std::size_t kBlockUncompressedSize = 12;
constexpr std::size_t kBlockCrc32 = 16;
constexpr std::size_t kBlockCodec = 20;
constexpr std::size_t kBlockFlags = 22;

class StringTableBuilder {
  public:
    std::uint32_t Add(std::string_view value) {
        const std::string key(value);
        const auto it = indices_.find(key);
        if (it != indices_.end()) {
            return it->second;
        }
        const auto index = static_cast<std::uint32_t>(values_.size());
        indices_.emplace(key, index);
        values_.emplace_back(value);
        return index;
    }

    [[nodiscard]] const std::vector<std::string>& Values() const noexcept { return values_; }

  private:
    std::map<std::string, std::uint32_t> indices_;
    std::vector<std::string> values_;
};

[[nodiscard]] bool HasNul(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] Result<void> ValidateToc(const BundleToc& toc, std::uint64_t logical_size) {
    if (toc.bundle_guid.IsZero()) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "bundle GUID is zero");
    }
    if (toc.bundle_name.empty() || HasNul(toc.bundle_name)) {
        return core::MakeError(core::ErrorCode::InvalidArgument,
                               "bundle name is empty or contains NUL");
    }

    std::unordered_set<core::AssetGuid, core::Guid128Hash> dependencies;
    for (const auto& dependency : toc.dependencies) {
        if (dependency.IsZero()) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "bundle dependency GUID is zero");
        }
        if (dependency == toc.bundle_guid) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "bundle cannot depend on itself");
        }
        if (!dependencies.insert(dependency).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "duplicate bundle dependency GUID");
        }
    }

    std::unordered_set<std::uint32_t> payload_ids;
    std::vector<ByteRange> ranges;
    ranges.reserve(toc.payloads.size() + toc.raw_files.size());
    for (const auto& payload : toc.payloads) {
        if (!payload_ids.insert(payload.payload_id).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument, "duplicate payload id");
        }
        if (payload.main_asset_guid.IsZero()) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "payload main asset GUID is zero");
        }
        if (payload.logical_offset > logical_size ||
            payload.size > logical_size - payload.logical_offset) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "payload range is outside the logical stream");
        }
        ranges.push_back(ByteRange{payload.logical_offset, payload.size});
    }

    std::unordered_set<std::string> raw_names;
    for (const auto& raw : toc.raw_files) {
        if (raw.name.empty() || HasNul(raw.name) ||
            !core::path::IsSafeRelative(raw.name)) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "raw file name is not a safe relative path");
        }
        if (!raw_names.insert(raw.name).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "duplicate raw file name");
        }
        if (raw.logical_offset > logical_size ||
            raw.size > logical_size - raw.logical_offset) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "raw file range is outside the logical stream");
        }
        ranges.push_back(ByteRange{raw.logical_offset, raw.size});
    }
    return CheckNoOverlap(ranges, "bundle payload/raw");
}

[[nodiscard]] Result<std::vector<std::byte>> BuildTocRaw(const BundleToc& toc) {
    StringTableBuilder strings;
    const std::uint32_t bundle_name_index = strings.Add(toc.bundle_name);
    std::vector<std::uint32_t> raw_name_indices;
    raw_name_indices.reserve(toc.raw_files.size());
    for (const auto& raw : toc.raw_files) {
        raw_name_indices.push_back(strings.Add(raw.name));
    }

    BinaryWriter writer(256);
    PARCEL_TRY(writer.WriteU32(kBundleVersion));
    PARCEL_TRY(writer.WriteGuid128(toc.bundle_guid));
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(strings.Values().size())));
    for (const auto& value : strings.Values()) {
        PARCEL_TRY(writer.WriteString(value));
    }
    PARCEL_TRY(writer.WriteU32(bundle_name_index));
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(toc.dependencies.size())));
    for (const auto& dependency : toc.dependencies) {
        PARCEL_TRY(writer.WriteGuid128(dependency));
    }
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(toc.payloads.size())));
    for (const auto& payload : toc.payloads) {
        PARCEL_TRY(writer.WriteU32(payload.payload_id));
        PARCEL_TRY(writer.WriteU64(payload.logical_offset));
        PARCEL_TRY(writer.WriteU64(payload.size));
        PARCEL_TRY(writer.WriteGuid128(payload.main_asset_guid));
        PARCEL_TRY(writer.WriteU32(payload.flags));
    }
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(toc.raw_files.size())));
    for (std::size_t i = 0; i < toc.raw_files.size(); ++i) {
        const auto& raw = toc.raw_files[i];
        PARCEL_TRY(writer.WriteU32(raw_name_indices[i]));
        PARCEL_TRY(writer.WriteU64(raw.logical_offset));
        PARCEL_TRY(writer.WriteU64(raw.size));
        PARCEL_TRY(writer.WriteU32(raw.flags));
    }
    return writer.Release();
}

[[nodiscard]] Result<void> WriteHeader(BinaryWriter& writer, const BundleHeader& header) {
    PARCEL_TRY(writer.PatchBytes(kHeaderMagic,
                                 std::as_bytes(std::span(kBundleMagic, sizeof(kBundleMagic)))));
    PARCEL_TRY(writer.PatchU16(kHeaderVersion, header.version));
    PARCEL_TRY(writer.PatchU16(kHeaderSize, header.header_size));
    PARCEL_TRY(writer.PatchU32(kHeaderFlags, header.flags));
    PARCEL_TRY(writer.PatchU64(kHeaderFileSize, header.file_size));
    PARCEL_TRY(writer.PatchU64(kHeaderBlockTableOffset, header.block_table_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderBlockTableSize, header.block_table_size));
    PARCEL_TRY(writer.PatchU32(kHeaderBlockCount, header.block_count));
    PARCEL_TRY(writer.PatchU64(kHeaderTocOffset, header.toc_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderTocCompressedSize, header.toc_compressed_size));
    PARCEL_TRY(writer.PatchU32(kHeaderTocRawSize, header.toc_raw_size));
    PARCEL_TRY(writer.PatchU32(kHeaderBlockSize, header.block_size));
    PARCEL_TRY(writer.PatchU8(kHeaderTocCodec,
                              static_cast<std::uint8_t>(header.toc_codec)));
    PARCEL_TRY(writer.PatchU8(kHeaderFileCodec,
                              static_cast<std::uint8_t>(header.file_codec)));
    PARCEL_TRY(writer.PatchU16(58, 0));
    PARCEL_TRY(writer.PatchHash256(kHeaderFileHash, header.file_hash));
    PARCEL_TRY(writer.PatchU32(kHeaderHeaderCrc, header.header_crc));
    return {};
}

} // namespace

BundleWriter::BundleWriter(BundleToc toc, BundleWriteOptions options)
    : toc_(std::move(toc)), options_(options) {}

Result<std::vector<std::byte>> BundleWriter::Build(
    std::span<const std::byte> logical_payload) const {
    return WriteBundle(toc_, logical_payload, options_);
}

Result<std::vector<std::byte>> WriteBundle(const BundleToc& toc,
                                           std::span<const std::byte> logical_payload,
                                           const BundleWriteOptions& options) {
    if (options.block_size == 0 || options.block_size > kMaxBlockSize) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "invalid bundle block size");
    }
    PARCEL_TRY(ValidateToc(toc, logical_payload.size()));

    auto toc_raw = BuildTocRaw(toc);
    if (!toc_raw) {
        return std::move(toc_raw).TakeError();
    }
    if (toc_raw.Value().size() > kMaxTocRawSize ||
        toc_raw.Value().size() > std::numeric_limits<std::uint32_t>::max()) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "bundle TOC is too large");
    }
    const std::uint32_t toc_raw_size = static_cast<std::uint32_t>(toc_raw.Value().size());

    CompressionCodec toc_codec = options.toc_codec;
    std::vector<std::byte> toc_stored;
    if (toc_codec == CompressionCodec::None) {
        toc_stored = std::move(toc_raw).Value();
    } else {
        auto compressed = Compress(toc_codec, toc_raw.Value(),
                                   CompressionOptions{options.compression_level});
        if (!compressed) {
            return std::move(compressed).TakeError();
        }
        if (compressed.Value().size() < toc_raw.Value().size()) {
            toc_stored = std::move(compressed).Value();
        } else {
            toc_codec = CompressionCodec::None;
            toc_stored = std::move(toc_raw).Value();
        }
    }
    if (toc_stored.size() > std::numeric_limits<std::uint32_t>::max()) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "bundle TOC is too large");
    }

    const std::uint64_t logical_size = logical_payload.size();
    if (logical_size > std::numeric_limits<std::uint64_t>::max() -
                          (static_cast<std::uint64_t>(options.block_size) - 1U)) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "bundle logical size overflows");
    }
    const std::uint64_t block_count =
        logical_size == 0 ? 0 : (logical_size + options.block_size - 1) / options.block_size;
    if (block_count > std::numeric_limits<std::uint32_t>::max() ||
        block_count > kMaxTableEntries) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "too many bundle blocks");
    }
    const std::uint64_t block_table_size = block_count * kBlockEntrySize;
    if (block_table_size > std::numeric_limits<std::uint32_t>::max() ||
        block_table_size > kMaxTableBytes) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "bundle block table is too large");
    }

    BinaryWriter writer(static_cast<std::size_t>(block_table_size) + toc_stored.size() + 256);
    PARCEL_TRY(writer.WriteZero(kBundleHeaderSize));

    BundleHeader header;
    header.block_table_offset = writer.Tell();
    header.block_table_size = static_cast<std::uint32_t>(block_table_size);
    header.block_count = static_cast<std::uint32_t>(block_count);
    header.block_size = options.block_size;
    header.file_codec = options.file_codec;
    header.toc_codec = toc_codec;
    header.flags = toc_codec == CompressionCodec::None ? 0U : kBundleFlagCompressedToc;
    for (std::uint64_t i = 0; i < block_count; ++i) {
        PARCEL_TRY(writer.WriteZero(kBlockEntrySize));
    }

    header.toc_offset = writer.Tell();
    header.toc_raw_size = toc_raw_size;
    header.toc_compressed_size = static_cast<std::uint32_t>(toc_stored.size());
    PARCEL_TRY(writer.WriteBytes(toc_stored));

    for (std::uint64_t i = 0; i < block_count; ++i) {
        PARCEL_TRY(writer.Align(8));
        const std::uint64_t logical_offset = i * options.block_size;
        const std::size_t raw_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(options.block_size, logical_size - logical_offset));
        const auto raw = logical_payload.subspan(static_cast<std::size_t>(logical_offset),
                                                 raw_size);
        const std::uint32_t crc = core::Crc32(raw);
        CompressionCodec block_codec = options.file_codec;
        std::vector<std::byte> stored;
        if (block_codec == CompressionCodec::None) {
            stored.assign(raw.begin(), raw.end());
        } else {
            auto compressed = Compress(block_codec, raw,
                                       CompressionOptions{options.compression_level});
            if (!compressed) {
                return std::move(compressed).TakeError();
            }
            if (compressed.Value().size() < raw.size()) {
                stored = std::move(compressed).Value();
            } else {
                block_codec = CompressionCodec::None;
                stored.assign(raw.begin(), raw.end());
            }
        }
        if (stored.size() > std::numeric_limits<std::uint32_t>::max()) {
            return core::MakeError(core::ErrorCode::InvalidArgument, "bundle block is too large");
        }

        const std::uint64_t data_offset = writer.Tell();
        PARCEL_TRY(writer.WriteBytes(stored));
        const std::size_t entry = static_cast<std::size_t>(header.block_table_offset) +
                                  static_cast<std::size_t>(i) * kBlockEntrySize;
        PARCEL_TRY(writer.PatchU64(entry + kBlockDataOffset, data_offset));
        PARCEL_TRY(writer.PatchU32(entry + kBlockCompressedSize,
                                   static_cast<std::uint32_t>(stored.size())));
        PARCEL_TRY(writer.PatchU32(entry + kBlockUncompressedSize,
                                   static_cast<std::uint32_t>(raw_size)));
        PARCEL_TRY(writer.PatchU32(entry + kBlockCrc32, crc));
        PARCEL_TRY(writer.PatchU16(entry + kBlockCodec,
                                   static_cast<std::uint16_t>(block_codec)));
        PARCEL_TRY(writer.PatchU16(entry + kBlockFlags, 0));
    }

    header.file_size = writer.Size();
    header.file_hash = {};
    header.header_crc = 0;
    PARCEL_TRY(WriteHeader(writer, header));

    const core::Hash256 file_hash = core::Sha256(writer.Data());
    header.file_hash = file_hash;
    PARCEL_TRY(writer.PatchHash256(kHeaderFileHash, file_hash));

    const std::uint32_t header_crc =
        core::Crc32(writer.Data().first(kBundleHeaderSize));
    header.header_crc = header_crc;
    PARCEL_TRY(writer.PatchU32(kHeaderHeaderCrc, header_crc));
    return writer.Release();
}

BundleReader::BundleReader(std::span<const std::byte> data) noexcept : data_(data) {}

Result<void> BundleReader::ReadHeader() {
    if (data_.size() < kBundleHeaderSize) {
        return core::MakeError(core::ErrorCode::Corrupt, "bundle file is too small");
    }

    BinaryReader reader(data_.first(kBundleHeaderSize));
    auto magic = reader.ReadBytes(4);
    if (!magic) {
        return std::move(magic).TakeError();
    }
    if (std::memcmp(magic.Value().data(), kBundleMagic, sizeof(kBundleMagic)) != 0) {
        return core::MakeError(core::ErrorCode::Corrupt, "invalid bundle file magic");
    }
    PARCEL_TRY_DECL(std::uint16_t, version, reader.ReadU16());
    PARCEL_TRY_DECL(std::uint16_t, header_size, reader.ReadU16());
    PARCEL_TRY_DECL(std::uint32_t, flags, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, file_size, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint64_t, block_table_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, block_table_size, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint32_t, block_count, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, toc_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, toc_compressed_size, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint32_t, toc_raw_size, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint32_t, block_size, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint8_t, toc_codec_value, reader.ReadU8());
    PARCEL_TRY_DECL(std::uint8_t, file_codec_value, reader.ReadU8());
    PARCEL_TRY(reader.Skip(2));
    PARCEL_TRY_DECL(core::Hash256, file_hash, reader.ReadHash256());
    PARCEL_TRY_DECL(std::uint32_t, header_crc, reader.ReadU32());
    PARCEL_TRY(reader.Skip(32));

    PARCEL_TRY(CheckVersion(version, kBundleVersion, "PRCL"));
    if (header_size != kBundleHeaderSize) {
        return core::MakeError(core::ErrorCode::Unsupported, "unsupported PRCL header size");
    }
    if (file_size != data_.size()) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               "PRCL file size does not match the header");
    }
    auto toc_codec = ParseCompressionCodec(toc_codec_value);
    if (!toc_codec) {
        return std::move(toc_codec).TakeError();
    }
    auto file_codec = ParseCompressionCodec(file_codec_value);
    if (!file_codec) {
        return std::move(file_codec).TakeError();
    }
    if (block_size == 0 || block_size > kMaxBlockSize) {
        return core::MakeError(core::ErrorCode::Corrupt, "invalid bundle block size");
    }
    if (block_count > kMaxTableEntries || block_table_size > kMaxTableBytes ||
        static_cast<std::uint64_t>(block_count) * kBlockEntrySize != block_table_size) {
        return core::MakeError(core::ErrorCode::Corrupt, "invalid bundle block table");
    }
    if (toc_raw_size == 0 || toc_raw_size > kMaxTocRawSize ||
        toc_compressed_size > kMaxTocRawSize) {
        return core::MakeError(core::ErrorCode::Corrupt, "invalid bundle TOC size");
    }
    if (toc_codec.Value() == CompressionCodec::None &&
        toc_compressed_size != toc_raw_size) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               "uncompressed TOC size does not match");
    }
    if (block_table_size != 0 && block_table_offset < header_size) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               "bundle block table overlaps the header");
    }
    if (toc_offset < header_size) {
        return core::MakeError(core::ErrorCode::Corrupt, "bundle TOC overlaps the header");
    }
    PARCEL_TRY(CheckTableRange(file_size, block_table_offset, block_count, kBlockEntrySize, 1,
                                "bundle block table"));
    PARCEL_TRY(CheckRange(file_size, toc_offset, toc_compressed_size, 1, "bundle TOC"));

    header_.version = version;
    header_.header_size = header_size;
    header_.flags = flags;
    header_.file_size = file_size;
    header_.block_table_offset = block_table_offset;
    header_.block_table_size = block_table_size;
    header_.block_count = block_count;
    header_.toc_offset = toc_offset;
    header_.toc_compressed_size = toc_compressed_size;
    header_.toc_raw_size = toc_raw_size;
    header_.block_size = block_size;
    header_.toc_codec = toc_codec.Value();
    header_.file_codec = file_codec.Value();
    header_.file_hash = file_hash;
    header_.header_crc = header_crc;
    header_read_ = true;
    return VerifyHeaderCrc();
}

Result<void> BundleReader::VerifyHeaderCrc() const {
    if (!header_read_) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "read the bundle header first");
    }
    std::vector<std::byte> header_bytes(data_.begin(),
                                        data_.begin() + kBundleHeaderSize);
    std::fill(header_bytes.begin() + kHeaderHeaderCrc,
              header_bytes.begin() + kHeaderHeaderCrc + 4, std::byte{0});
    const std::uint32_t crc = core::Crc32(header_bytes);
    if (crc != header_.header_crc) {
        return core::MakeError(core::ErrorCode::Corrupt, "bundle header CRC mismatch");
    }
    return {};
}

Result<void> BundleReader::ReadBlocks() {
    if (!header_read_) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "read the bundle header first");
    }
    std::vector<BundleBlock> blocks;
    blocks.reserve(header_.block_count);
    std::vector<ByteRange> ranges;
    ranges.push_back(ByteRange{0, header_.header_size});
    ranges.push_back(ByteRange{header_.block_table_offset, header_.block_table_size});
    ranges.push_back(ByteRange{header_.toc_offset, header_.toc_compressed_size});

    std::uint64_t logical_size = 0;
    for (std::uint32_t i = 0; i < header_.block_count; ++i) {
        const std::size_t entry = static_cast<std::size_t>(header_.block_table_offset) +
                                  static_cast<std::size_t>(i) * kBlockEntrySize;
        BinaryReader reader(data_.subspan(entry, kBlockEntrySize));
        PARCEL_TRY_DECL(std::uint64_t, data_offset, reader.ReadU64());
        PARCEL_TRY_DECL(std::uint32_t, compressed_size, reader.ReadU32());
        PARCEL_TRY_DECL(std::uint32_t, uncompressed_size, reader.ReadU32());
        PARCEL_TRY_DECL(std::uint32_t, crc32, reader.ReadU32());
        PARCEL_TRY_DECL(std::uint16_t, codec_value, reader.ReadU16());
        PARCEL_TRY_DECL(std::uint16_t, flags, reader.ReadU16());

        if (uncompressed_size > header_.block_size || compressed_size > kMaxCompressedBlockSize) {
            return core::MakeError(core::ErrorCode::Corrupt, "bundle block size is invalid");
        }
        if (uncompressed_size == 0 ||
            (i + 1U < header_.block_count && uncompressed_size != header_.block_size)) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "bundle block does not cover a full logical block");
        }
        if (codec_value > 255U) {
            return core::MakeError(core::ErrorCode::Corrupt, "bundle block codec is invalid");
        }
        auto codec = ParseCompressionCodec(static_cast<std::uint8_t>(codec_value));
        if (!codec) {
            return std::move(codec).TakeError();
        }
        if (codec.Value() == CompressionCodec::None && compressed_size != uncompressed_size) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "uncompressed block size does not match");
        }
        PARCEL_TRY(CheckRange(header_.file_size, data_offset, compressed_size, 1,
                               "bundle block data"));
        if (logical_size > std::numeric_limits<std::uint64_t>::max() - uncompressed_size) {
            return core::MakeError(core::ErrorCode::Corrupt, "bundle logical size overflows");
        }
        logical_size += uncompressed_size;
        ranges.push_back(ByteRange{data_offset, compressed_size});
        blocks.push_back(BundleBlock{data_offset, compressed_size, uncompressed_size, crc32,
                                     codec.Value(), flags});
    }
    PARCEL_TRY(CheckNoOverlap(ranges, "bundle block"));
    blocks_ = std::move(blocks);
    logical_size_ = logical_size;
    return {};
}

Result<void> BundleReader::ReadToc() {
    if (!header_read_) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "read the bundle header first");
    }
    const auto compressed = data_.subspan(static_cast<std::size_t>(header_.toc_offset),
                                          header_.toc_compressed_size);
    auto toc_raw_bytes = Decompress(header_.toc_codec, compressed, header_.toc_raw_size);
    if (!toc_raw_bytes) {
        return std::move(toc_raw_bytes).TakeError();
    }
    toc_raw_ = std::move(toc_raw_bytes).Value();

    BinaryReader reader(toc_raw_);
    PARCEL_TRY_DECL(std::uint32_t, toc_version, reader.ReadU32());
    if (toc_version != kBundleVersion) {
        return core::MakeError(core::ErrorCode::Unsupported, "PRCL TOC version is not supported");
    }
    PARCEL_TRY_DECL(core::AssetGuid, bundle_guid, reader.ReadGuid128());
    if (bundle_guid.IsZero()) {
        return core::MakeError(core::ErrorCode::Corrupt, "bundle TOC GUID is zero");
    }
    PARCEL_TRY_DECL(std::uint32_t, string_count, reader.ReadU32());
    if (string_count > kMaxTableEntries) {
        return core::MakeError(core::ErrorCode::Corrupt, "bundle TOC string table is too large");
    }
    std::vector<std::string> strings;
    strings.reserve(string_count);
    for (std::uint32_t i = 0; i < string_count; ++i) {
        auto value = reader.ReadString();
        if (!value) {
            return std::move(value).TakeError();
        }
        if (HasNul(value.Value())) {
            return core::MakeError(core::ErrorCode::Corrupt, "bundle TOC string contains NUL");
        }
        strings.emplace_back(value.Value());
    }
    PARCEL_TRY_DECL(std::uint32_t, bundle_name_index, reader.ReadU32());
    if (bundle_name_index >= strings.size() || strings[bundle_name_index].empty()) {
        return core::MakeError(core::ErrorCode::Corrupt, "invalid bundle name index");
    }
    BundleToc toc;
    toc.bundle_guid = bundle_guid;
    toc.bundle_name = strings[bundle_name_index];

    PARCEL_TRY_DECL(std::uint32_t, dependency_count, reader.ReadU32());
    if (dependency_count > kMaxTableEntries) {
        return core::MakeError(core::ErrorCode::Corrupt, "bundle dependency table is too large");
    }
    std::unordered_set<core::AssetGuid, core::Guid128Hash> dependencies;
    for (std::uint32_t i = 0; i < dependency_count; ++i) {
        PARCEL_TRY_DECL(core::AssetGuid, dependency, reader.ReadGuid128());
        if (dependency.IsZero() || dependency == bundle_guid ||
            !dependencies.insert(dependency).second) {
            return core::MakeError(core::ErrorCode::Corrupt, "invalid bundle dependency");
        }
        toc.dependencies.push_back(dependency);
    }

    PARCEL_TRY_DECL(std::uint32_t, payload_count, reader.ReadU32());
    if (payload_count > kMaxTableEntries) {
        return core::MakeError(core::ErrorCode::Corrupt, "bundle payload table is too large");
    }
    std::unordered_set<std::uint32_t> payload_ids;
    for (std::uint32_t i = 0; i < payload_count; ++i) {
        BundlePayloadDesc payload;
        PARCEL_TRY_ASSIGN(payload.payload_id, reader.ReadU32());
        PARCEL_TRY_ASSIGN(payload.logical_offset, reader.ReadU64());
        PARCEL_TRY_ASSIGN(payload.size, reader.ReadU64());
        PARCEL_TRY_ASSIGN(payload.main_asset_guid, reader.ReadGuid128());
        PARCEL_TRY_ASSIGN(payload.flags, reader.ReadU32());
        if (!payload_ids.insert(payload.payload_id).second ||
            payload.main_asset_guid.IsZero() ||
            payload.logical_offset > logical_size_ ||
            payload.size > logical_size_ - payload.logical_offset) {
            return core::MakeError(core::ErrorCode::Corrupt, "invalid bundle payload record");
        }
        toc.payloads.push_back(payload);
    }

    PARCEL_TRY_DECL(std::uint32_t, raw_count, reader.ReadU32());
    if (raw_count > kMaxTableEntries) {
        return core::MakeError(core::ErrorCode::Corrupt, "bundle raw file table is too large");
    }
    std::unordered_set<std::string> raw_names;
    for (std::uint32_t i = 0; i < raw_count; ++i) {
        BundleRawFileDesc raw;
        PARCEL_TRY_DECL(std::uint32_t, name_index, reader.ReadU32());
        if (name_index >= strings.size()) {
            return core::MakeError(core::ErrorCode::Corrupt, "invalid raw file name index");
        }
        raw.name = strings[name_index];
        PARCEL_TRY_ASSIGN(raw.logical_offset, reader.ReadU64());
        PARCEL_TRY_ASSIGN(raw.size, reader.ReadU64());
        PARCEL_TRY_ASSIGN(raw.flags, reader.ReadU32());
        if (raw.name.empty() || HasNul(raw.name) ||
            !core::path::IsSafeRelative(raw.name) ||
            !raw_names.insert(raw.name).second ||
            raw.logical_offset > logical_size_ ||
            raw.size > logical_size_ - raw.logical_offset) {
            return core::MakeError(core::ErrorCode::Corrupt, "invalid raw file record");
        }
        toc.raw_files.push_back(std::move(raw));
    }
    if (!reader.IsAtEnd()) {
        return core::MakeError(core::ErrorCode::Corrupt, "bundle TOC has trailing bytes");
    }

    std::vector<ByteRange> ranges;
    ranges.reserve(toc.payloads.size() + toc.raw_files.size());
    for (const auto& payload : toc.payloads) {
        ranges.push_back(ByteRange{payload.logical_offset, payload.size});
    }
    for (const auto& raw : toc.raw_files) {
        ranges.push_back(ByteRange{raw.logical_offset, raw.size});
    }
    PARCEL_TRY(CheckNoOverlap(ranges, "bundle TOC payload/raw"));
    toc_ = std::move(toc);
    return {};
}

Result<void> BundleReader::VerifyFileHash() const {
    if (!header_read_) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "read the bundle header first");
    }
    std::vector<std::byte> copy(data_.begin(), data_.end());
    std::fill(copy.begin() + kHeaderFileHash, copy.begin() + kHeaderFileHash + 32, std::byte{0});
    std::fill(copy.begin() + kHeaderHeaderCrc, copy.begin() + kHeaderHeaderCrc + 4, std::byte{0});
    const core::Hash256 actual = core::Sha256(copy);
    if (actual != header_.file_hash) {
        return core::MakeError(core::ErrorCode::HashMismatch, "bundle file hash mismatch");
    }
    return {};
}

Result<void> BundleReader::ReadAll(bool verify_file_hash) {
    PARCEL_TRY(ReadHeader());
    PARCEL_TRY(ReadBlocks());
    PARCEL_TRY(ReadToc());
    if (verify_file_hash) {
        PARCEL_TRY(VerifyFileHash());
    }
    return {};
}

Result<const std::vector<std::byte>*> BundleReader::LoadBlock(std::uint32_t index) {
    if (index >= blocks_.size()) {
        return core::MakeError(core::ErrorCode::Corrupt, "bundle block index is out of range");
    }
    if (cached_block_index_ == index) {
        return &cached_block_;
    }
    const BundleBlock& block = blocks_[index];
    const auto compressed = data_.subspan(static_cast<std::size_t>(block.data_offset),
                                          block.compressed_size);
    auto raw = Decompress(block.codec, compressed, block.uncompressed_size);
    if (!raw) {
        return std::move(raw).TakeError();
    }
    if (core::Crc32(raw.Value()) != block.crc32) {
        return core::MakeError(core::ErrorCode::HashMismatch, "bundle block CRC mismatch");
    }
    cached_block_ = std::move(raw).Value();
    cached_block_index_ = index;
    return &cached_block_;
}

Result<std::vector<std::byte>> BundleReader::ReadLogical(std::uint64_t offset,
                                                         std::uint64_t size) {
    if (offset > logical_size_ || size > logical_size_ - offset) {
        return core::MakeError(core::ErrorCode::InvalidArgument,
                               "logical read range is out of bounds");
    }
    if (size == 0) {
        return std::vector<std::byte>{};
    }
    if (size > std::numeric_limits<std::size_t>::max()) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "logical read is too large");
    }

    std::vector<std::byte> result(static_cast<std::size_t>(size));
    std::uint64_t cursor = offset;
    const std::uint64_t end = offset + size;
    std::size_t destination = 0;
    while (cursor < end) {
        const std::uint64_t block_index = cursor / header_.block_size;
        const std::uint64_t block_start = block_index * header_.block_size;
        if (block_index >= blocks_.size()) {
            return core::MakeError(core::ErrorCode::Corrupt, "bundle logical block is missing");
        }
        auto loaded = LoadBlock(static_cast<std::uint32_t>(block_index));
        if (!loaded) {
            return std::move(loaded).TakeError();
        }
        const std::uint64_t local_offset = cursor - block_start;
        if (local_offset >= loaded.Value()->size()) {
            return core::MakeError(core::ErrorCode::Corrupt, "bundle block has an invalid size");
        }
        const std::uint64_t available = loaded.Value()->size() - local_offset;
        const std::size_t copy_size =
            static_cast<std::size_t>(std::min<std::uint64_t>(available, end - cursor));
        std::memcpy(result.data() + destination,
                    loaded.Value()->data() + static_cast<std::size_t>(local_offset), copy_size);
        destination += copy_size;
        cursor += copy_size;
    }
    return result;
}

Result<std::vector<std::byte>> BundleReader::ReadAllPayload() {
    if (!header_read_) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "read the bundle header first");
    }
    return ReadLogical(0, logical_size_);
}

Result<BundleReader> BundleReader::Open(std::span<const std::byte> data,
                                        bool verify_file_hash) {
    BundleReader reader(data);
    PARCEL_TRY(reader.ReadAll(verify_file_hash));
    return reader;
}

} // namespace parcel::serialize
