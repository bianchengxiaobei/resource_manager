#include "parcel/serialize/asset_file.hpp"

#include "parcel/core/hash.hpp"
#include "parcel/core/path.hpp"
#include "parcel/serialize/binary.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace parcel::serialize {
namespace {

constexpr std::size_t kTypeEntrySize = 16;
constexpr std::size_t kObjectEntrySize = 40;
constexpr std::size_t kExternalRefEntrySize = 20;
constexpr std::size_t kSubAssetEntrySize = 12;
constexpr std::size_t kBlobEntrySize = 24;

constexpr std::size_t kHeaderMagic = 0;
constexpr std::size_t kHeaderVersion = 4;
constexpr std::size_t kHeaderSize = 6;
constexpr std::size_t kHeaderFlags = 8;
constexpr std::size_t kHeaderFileSize = 12;
constexpr std::size_t kHeaderTypeTableOffset = 20;
constexpr std::size_t kHeaderTypeCount = 28;
constexpr std::size_t kHeaderObjectTableOffset = 32;
constexpr std::size_t kHeaderObjectCount = 40;
constexpr std::size_t kHeaderExternalRefOffset = 44;
constexpr std::size_t kHeaderExternalRefCount = 52;
constexpr std::size_t kHeaderSubAssetOffset = 56;
constexpr std::size_t kHeaderSubAssetCount = 64;
constexpr std::size_t kHeaderFlatbufferOffset = 68;
constexpr std::size_t kHeaderFlatbufferSize = 76;
constexpr std::size_t kHeaderBlobTableOffset = 80;
constexpr std::size_t kHeaderBlobCount = 88;
constexpr std::size_t kHeaderContentHash = 92;

constexpr std::size_t kTypeId = 0;
constexpr std::size_t kTypeNameOffset = 4;
constexpr std::size_t kTypeSerializerVersion = 8;
constexpr std::size_t kTypeFlags = 12;

constexpr std::size_t kObjectLocalId = 0;
constexpr std::size_t kObjectTypeId = 4;
constexpr std::size_t kObjectMetadataOffset = 8;
constexpr std::size_t kObjectMetadataSize = 16;
constexpr std::size_t kObjectBlobId = 24;
constexpr std::size_t kObjectFlags = 32;

constexpr std::size_t kExternalRefGuid = 0;
constexpr std::size_t kExternalRefFlags = 16;

constexpr std::size_t kSubAssetNameOffset = 0;
constexpr std::size_t kSubAssetLocalObjectId = 4;
constexpr std::size_t kSubAssetFlags = 8;

constexpr std::size_t kBlobOffset = 0;
constexpr std::size_t kBlobSize = 8;
constexpr std::size_t kBlobCrc32 = 16;
constexpr std::size_t kBlobCodec = 20;
constexpr std::size_t kBlobFlags = 21;

[[nodiscard]] bool HasNul(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] Result<void> WriteHeader(BinaryWriter& writer, const AssetHeader& header) {
    PARCEL_TRY(writer.PatchBytes(kHeaderMagic,
                                 std::as_bytes(std::span(kAssetMagic, sizeof(kAssetMagic)))));
    PARCEL_TRY(writer.PatchU16(kHeaderVersion, header.version));
    PARCEL_TRY(writer.PatchU16(kHeaderSize, header.header_size));
    PARCEL_TRY(writer.PatchU32(kHeaderFlags, header.flags));
    PARCEL_TRY(writer.PatchU64(kHeaderFileSize, header.file_size));
    PARCEL_TRY(writer.PatchU64(kHeaderTypeTableOffset, header.type_table_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderTypeCount, header.type_count));
    PARCEL_TRY(writer.PatchU64(kHeaderObjectTableOffset, header.object_table_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderObjectCount, header.object_count));
    PARCEL_TRY(writer.PatchU64(kHeaderExternalRefOffset, header.external_ref_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderExternalRefCount, header.external_ref_count));
    PARCEL_TRY(writer.PatchU64(kHeaderSubAssetOffset, header.sub_asset_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderSubAssetCount, header.sub_asset_count));
    PARCEL_TRY(writer.PatchU64(kHeaderFlatbufferOffset, header.flatbuffer_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderFlatbufferSize, header.flatbuffer_size));
    PARCEL_TRY(writer.PatchU64(kHeaderBlobTableOffset, header.blob_table_offset));
    PARCEL_TRY(writer.PatchU32(kHeaderBlobCount, header.blob_count));
    PARCEL_TRY(writer.PatchHash256(kHeaderContentHash, header.content_hash));
    return {};
}

[[nodiscard]] Result<void> ValidateAssetDesc(const AssetFileDesc& desc) {
    if (desc.types.size() > kMaxTableEntries || desc.objects.size() > kMaxTableEntries ||
        desc.external_refs.size() > kMaxTableEntries ||
        desc.sub_assets.size() > kMaxTableEntries || desc.blobs.size() > kMaxTableEntries) {
        return core::MakeError(core::ErrorCode::InvalidArgument,
                               "asset table entry count exceeds the limit");
    }

    std::unordered_set<std::uint32_t> type_ids;
    std::unordered_set<std::string_view> type_names;
    for (const auto& type : desc.types) {
        if (type.type_name.empty() || HasNul(type.type_name)) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "asset type name is empty or contains NUL");
        }
        if (!type_ids.insert(type.type_id).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument, "duplicate asset type id");
        }
        if (!type_names.insert(type.type_name).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument, "duplicate asset type name");
        }
    }

    std::unordered_set<std::uint32_t> local_ids;
    std::uint64_t flatbuffer_size = 0;
    for (const auto& object : desc.objects) {
        if (!local_ids.insert(object.local_id).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument, "duplicate asset local id");
        }
        if (type_ids.find(object.type_id) == type_ids.end()) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "asset object references an unknown type");
        }
        if (object.blob_id != kNoBlob && object.blob_id >= desc.blobs.size()) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "asset object references an unknown blob");
        }
        if (object.metadata.size() >
            std::numeric_limits<std::uint64_t>::max() - flatbuffer_size) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "asset flatbuffer size overflows");
        }
        flatbuffer_size += object.metadata.size();
    }
    if (flatbuffer_size > std::numeric_limits<std::uint32_t>::max()) {
        return core::MakeError(core::ErrorCode::InvalidArgument,
                               "asset flatbuffer region exceeds 32-bit size");
    }

    for (const auto& reference : desc.external_refs) {
        if (reference.asset_guid.IsZero()) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "asset external reference GUID is zero");
        }
    }

    std::unordered_set<std::string_view> sub_asset_names;
    for (const auto& sub_asset : desc.sub_assets) {
        if (sub_asset.name.empty() || HasNul(sub_asset.name)) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "sub-asset name is empty or contains NUL");
        }
        if (!sub_asset_names.insert(sub_asset.name).second) {
            return core::MakeError(core::ErrorCode::InvalidArgument, "duplicate sub-asset name");
        }
        if (local_ids.find(sub_asset.local_object_id) == local_ids.end()) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "sub-asset references an unknown local object");
        }
    }
    return {};
}

[[nodiscard]] Result<void> PatchObjectEntry(BinaryWriter& writer, std::size_t table_offset,
                                            std::size_t index, const AssetFileDesc& desc,
                                            std::uint64_t metadata_offset) {
    const auto& object = desc.objects[index];
    const std::size_t entry = table_offset + index * kObjectEntrySize;
    PARCEL_TRY(writer.PatchU32(entry + kObjectLocalId, object.local_id));
    PARCEL_TRY(writer.PatchU32(entry + kObjectTypeId, object.type_id));
    PARCEL_TRY(writer.PatchU64(entry + kObjectMetadataOffset, metadata_offset));
    PARCEL_TRY(writer.PatchU64(
        entry + kObjectMetadataSize, static_cast<std::uint64_t>(object.metadata.size())));
    PARCEL_TRY(writer.PatchU64(entry + kObjectBlobId, object.blob_id));
    PARCEL_TRY(writer.PatchU32(entry + kObjectFlags, object.flags));
    return {};
}

} // namespace

void AssetFileWriter::AddType(AssetTypeDesc type) {
    desc_.types.push_back(std::move(type));
}

void AssetFileWriter::AddObject(AssetObjectDesc object) {
    desc_.objects.push_back(std::move(object));
}

void AssetFileWriter::AddExternalRef(AssetExternalRefDesc reference) {
    desc_.external_refs.push_back(reference);
}

void AssetFileWriter::AddSubAsset(AssetSubAssetDesc sub_asset) {
    desc_.sub_assets.push_back(std::move(sub_asset));
}

void AssetFileWriter::AddBlob(AssetBlobDesc blob) {
    desc_.blobs.push_back(std::move(blob));
}

Result<std::vector<std::byte>> AssetFileWriter::Build() const {
    return WriteAssetFile(desc_);
}

Result<std::vector<std::byte>> WriteAssetFile(const AssetFileDesc& desc) {
    PARCEL_TRY(ValidateAssetDesc(desc));

    BinaryWriter writer(1024);
    PARCEL_TRY(writer.WriteZero(kAssetHeaderSize));

    AssetHeader header;
    header.file_size = 0;
    header.type_table_offset = writer.Tell();
    header.type_count = static_cast<std::uint32_t>(desc.types.size());
    for (std::size_t i = 0; i < desc.types.size(); ++i) {
        PARCEL_TRY(writer.WriteZero(kTypeEntrySize));
    }
    for (std::size_t i = 0; i < desc.types.size(); ++i) {
        const auto& type = desc.types[i];
        const std::uint64_t name_offset = writer.Tell();
        PARCEL_TRY(writer.WriteCString(type.type_name));
        const std::size_t entry = header.type_table_offset + i * kTypeEntrySize;
        PARCEL_TRY(writer.PatchU32(entry + kTypeId, type.type_id));
        PARCEL_TRY(writer.PatchU32(entry + kTypeNameOffset,
                                   static_cast<std::uint32_t>(name_offset)));
        PARCEL_TRY(writer.PatchU32(entry + kTypeSerializerVersion, type.serializer_version));
        PARCEL_TRY(writer.PatchU32(entry + kTypeFlags, type.flags));
    }

    PARCEL_TRY(writer.Align(8));
    header.object_table_offset = writer.Tell();
    header.object_count = static_cast<std::uint32_t>(desc.objects.size());
    for (std::size_t i = 0; i < desc.objects.size(); ++i) {
        PARCEL_TRY(writer.WriteZero(kObjectEntrySize));
    }

    PARCEL_TRY(writer.Align(8));
    header.external_ref_offset = writer.Tell();
    header.external_ref_count = static_cast<std::uint32_t>(desc.external_refs.size());
    for (const auto& reference : desc.external_refs) {
        PARCEL_TRY(writer.WriteGuid128(reference.asset_guid));
        PARCEL_TRY(writer.WriteU32(reference.flags));
    }

    PARCEL_TRY(writer.Align(8));
    header.sub_asset_offset = writer.Tell();
    header.sub_asset_count = static_cast<std::uint32_t>(desc.sub_assets.size());
    for (std::size_t i = 0; i < desc.sub_assets.size(); ++i) {
        PARCEL_TRY(writer.WriteZero(kSubAssetEntrySize));
    }
    for (std::size_t i = 0; i < desc.sub_assets.size(); ++i) {
        const auto& sub_asset = desc.sub_assets[i];
        const std::uint64_t name_offset = writer.Tell();
        PARCEL_TRY(writer.WriteCString(sub_asset.name));
        const std::size_t entry = header.sub_asset_offset + i * kSubAssetEntrySize;
        PARCEL_TRY(writer.PatchU32(entry + kSubAssetNameOffset,
                                   static_cast<std::uint32_t>(name_offset)));
        PARCEL_TRY(writer.PatchU32(entry + kSubAssetLocalObjectId, sub_asset.local_object_id));
        PARCEL_TRY(writer.PatchU32(entry + kSubAssetFlags, sub_asset.flags));
    }

    PARCEL_TRY(writer.Align(8));
    header.flatbuffer_offset = writer.Tell();
    for (std::size_t i = 0; i < desc.objects.size(); ++i) {
        PARCEL_TRY(writer.Align(8));
        const std::uint64_t metadata_offset = writer.Tell();
        PARCEL_TRY(writer.WriteBytes(desc.objects[i].metadata));
        PARCEL_TRY(PatchObjectEntry(writer, static_cast<std::size_t>(header.object_table_offset), i,
                                    desc, metadata_offset));
    }
    header.flatbuffer_size =
        static_cast<std::uint32_t>(writer.Tell() - header.flatbuffer_offset);
    if (writer.Tell() - header.flatbuffer_offset >
        std::numeric_limits<std::uint32_t>::max()) {
        return core::MakeError(core::ErrorCode::InvalidArgument,
                               "asset flatbuffer region exceeds 32-bit size");
    }

    PARCEL_TRY(writer.Align(8));
    header.blob_table_offset = writer.Tell();
    header.blob_count = static_cast<std::uint32_t>(desc.blobs.size());
    for (std::size_t i = 0; i < desc.blobs.size(); ++i) {
        PARCEL_TRY(writer.WriteZero(kBlobEntrySize));
    }
    for (std::size_t i = 0; i < desc.blobs.size(); ++i) {
        const auto& blob = desc.blobs[i];
        PARCEL_TRY(writer.Align(8));
        const std::uint64_t offset = writer.Tell();
        PARCEL_TRY(writer.WriteBytes(blob.data));
        const std::uint32_t crc = core::Crc32(blob.data);
        const std::size_t entry = header.blob_table_offset + i * kBlobEntrySize;
        PARCEL_TRY(writer.PatchU64(entry + kBlobOffset, offset));
        PARCEL_TRY(writer.PatchU64(entry + kBlobSize,
                                   static_cast<std::uint64_t>(blob.data.size())));
        PARCEL_TRY(writer.PatchU32(entry + kBlobCrc32, crc));
        PARCEL_TRY(writer.PatchU8(entry + kBlobCodec,
                                  static_cast<std::uint8_t>(CompressionCodec::None)));
        PARCEL_TRY(writer.PatchU8(entry + kBlobFlags, 0));
        PARCEL_TRY(writer.PatchU16(entry + 22, 0));
    }

    header.file_size = writer.Size();
    header.content_hash = core::Sha256(writer.Data().subspan(kAssetHeaderSize));
    PARCEL_TRY(WriteHeader(writer, header));
    return writer.Release();
}

AssetFileReader::AssetFileReader(std::span<const std::byte> data) noexcept : data_(data) {}

Result<void> AssetFileReader::ReadHeader() {
    if (data_.size() < kAssetHeaderSize) {
        return core::MakeError(core::ErrorCode::Corrupt, "asset file is too small");
    }

    BinaryReader reader(data_.first(kAssetHeaderSize));
    auto magic = reader.ReadBytes(4);
    if (!magic) {
        return std::move(magic).TakeError();
    }
    if (std::memcmp(magic.Value().data(), kAssetMagic, sizeof(kAssetMagic)) != 0) {
        return core::MakeError(core::ErrorCode::Corrupt, "invalid asset file magic");
    }
    PARCEL_TRY_DECL(std::uint16_t, version, reader.ReadU16());
    PARCEL_TRY_DECL(std::uint16_t, header_size, reader.ReadU16());
    PARCEL_TRY_DECL(std::uint32_t, flags, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, file_size, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint64_t, type_table_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, type_count, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, object_table_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, object_count, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, external_ref_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, external_ref_count, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, sub_asset_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, sub_asset_count, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, flatbuffer_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, flatbuffer_size, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint64_t, blob_table_offset, reader.ReadU64());
    PARCEL_TRY_DECL(std::uint32_t, blob_count, reader.ReadU32());
    PARCEL_TRY_DECL(core::Hash256, content_hash, reader.ReadHash256());

    PARCEL_TRY(CheckVersion(version, kAssetVersion, "PRCA"));
    if (header_size != kAssetHeaderSize) {
        return core::MakeError(core::ErrorCode::Unsupported, "unsupported PRCA header size");
    }
    if (file_size != data_.size()) {
        return core::MakeError(core::ErrorCode::Corrupt, "PRCA file size does not match the header");
    }

    header_.version = version;
    header_.header_size = header_size;
    header_.flags = flags;
    header_.file_size = file_size;
    header_.type_table_offset = type_table_offset;
    header_.type_count = type_count;
    header_.object_table_offset = object_table_offset;
    header_.object_count = object_count;
    header_.external_ref_offset = external_ref_offset;
    header_.external_ref_count = external_ref_count;
    header_.sub_asset_offset = sub_asset_offset;
    header_.sub_asset_count = sub_asset_count;
    header_.flatbuffer_offset = flatbuffer_offset;
    header_.flatbuffer_size = flatbuffer_size;
    header_.blob_table_offset = blob_table_offset;
    header_.blob_count = blob_count;
    header_.content_hash = content_hash;

    const auto check_table = [&](std::uint64_t offset, std::uint64_t count,
                                 std::uint64_t entry_size,
                                 std::string_view what) -> Result<void> {
        if (count != 0 && offset < header_size) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   std::string(what) + " overlaps the header");
        }
        PARCEL_TRY(CheckTableRange(file_size, offset, count, entry_size, 1, what));
        return {};
    };

    PARCEL_TRY(check_table(header_.type_table_offset, header_.type_count, kTypeEntrySize,
                            "asset type table"));
    PARCEL_TRY(check_table(header_.object_table_offset, header_.object_count, kObjectEntrySize,
                            "asset object table"));
    PARCEL_TRY(check_table(header_.external_ref_offset, header_.external_ref_count,
                            kExternalRefEntrySize, "asset external reference table"));
    PARCEL_TRY(check_table(header_.sub_asset_offset, header_.sub_asset_count,
                            kSubAssetEntrySize, "asset sub-asset table"));
    PARCEL_TRY(check_table(header_.blob_table_offset, header_.blob_count, kBlobEntrySize,
                            "asset blob table"));
    if (header_.flatbuffer_size != 0 && header_.flatbuffer_offset < header_size) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               "asset flatbuffer region overlaps the header");
    }
    PARCEL_TRY(CheckRange(file_size, header_.flatbuffer_offset, header_.flatbuffer_size, 1,
                           "asset flatbuffer region"));

    header_read_ = true;
    return {};
}

Result<void> AssetFileReader::ReadTypes() {
    if (!header_read_) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "read the asset header first");
    }
    std::vector<AssetTypeView> types;
    types.reserve(header_.type_count);
    std::unordered_set<std::uint32_t> type_ids;
    std::unordered_set<std::string_view> type_names;
    for (std::uint32_t i = 0; i < header_.type_count; ++i) {
        const std::size_t entry = static_cast<std::size_t>(header_.type_table_offset) +
                                  static_cast<std::size_t>(i) * kTypeEntrySize;
        BinaryReader reader(data_.subspan(entry, kTypeEntrySize));
        PARCEL_TRY_DECL(std::uint32_t, type_id, reader.ReadU32());
        PARCEL_TRY_DECL(std::uint32_t, name_offset, reader.ReadU32());
        PARCEL_TRY_DECL(std::uint32_t, serializer_version, reader.ReadU32());
        PARCEL_TRY_DECL(std::uint32_t, flags, reader.ReadU32());
        if (name_offset >= header_.file_size) {
            return core::MakeError(core::ErrorCode::Corrupt, "asset type name offset is invalid");
        }
        auto name = BinaryReader(data_.subspan(name_offset)).ReadCString();
        if (!name) {
            return std::move(name).TakeError();
        }
        if (name.Value().empty()) {
            return core::MakeError(core::ErrorCode::Corrupt, "asset type name is empty");
        }
        if (!type_ids.insert(type_id).second || !type_names.insert(name.Value()).second) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "duplicate asset type id or name");
        }
        types.push_back(AssetTypeView{type_id, name.Value(), serializer_version, flags});
    }
    types_ = std::move(types);
    return {};
}

Result<void> AssetFileReader::ReadObjects() {
    if (!header_read_) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "read the asset header first");
    }
    std::vector<AssetObjectView> objects;
    objects.reserve(header_.object_count);
    std::unordered_set<std::uint32_t> local_ids;
    for (std::uint32_t i = 0; i < header_.object_count; ++i) {
        const std::size_t entry = static_cast<std::size_t>(header_.object_table_offset) +
                                  static_cast<std::size_t>(i) * kObjectEntrySize;
        BinaryReader reader(data_.subspan(entry, kObjectEntrySize));
        PARCEL_TRY_DECL(std::uint32_t, local_id, reader.ReadU32());
        PARCEL_TRY_DECL(std::uint32_t, type_id, reader.ReadU32());
        PARCEL_TRY_DECL(std::uint64_t, metadata_offset, reader.ReadU64());
        PARCEL_TRY_DECL(std::uint64_t, metadata_size, reader.ReadU64());
        PARCEL_TRY_DECL(std::uint64_t, blob_id, reader.ReadU64());
        PARCEL_TRY_DECL(std::uint32_t, flags, reader.ReadU32());
        PARCEL_TRY(reader.Skip(4));

        if (!local_ids.insert(local_id).second) {
            return core::MakeError(core::ErrorCode::Corrupt, "duplicate asset local id");
        }
        if (blob_id != kNoBlob && blob_id >= header_.blob_count) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "asset object references an unknown blob");
        }
        if (metadata_size > std::numeric_limits<std::size_t>::max()) {
            return core::MakeError(core::ErrorCode::Corrupt, "asset metadata is too large");
        }
        if (metadata_size != 0) {
            auto range = CheckRange(header_.file_size, metadata_offset, metadata_size, 1,
                                    "asset object metadata");
            if (!range) {
                return std::move(range).TakeError();
            }
            if (header_.flatbuffer_size == 0 ||
                metadata_offset < header_.flatbuffer_offset ||
                range.Value().End() > header_.flatbuffer_offset + header_.flatbuffer_size) {
                return core::MakeError(core::ErrorCode::Corrupt,
                                       "asset object metadata is outside the flatbuffer region");
            }
        }
        if (metadata_offset > header_.file_size) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "asset object metadata offset is invalid");
        }
        objects.push_back(AssetObjectView{
            local_id,
            type_id,
            data_.subspan(static_cast<std::size_t>(metadata_offset),
                          static_cast<std::size_t>(metadata_size)),
            blob_id,
            flags,
        });
    }
    objects_ = std::move(objects);
    return {};
}

Result<void> AssetFileReader::ReadExternalRefs() {
    if (!header_read_) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "read the asset header first");
    }
    std::vector<AssetExternalRefView> refs;
    refs.reserve(header_.external_ref_count);
    for (std::uint32_t i = 0; i < header_.external_ref_count; ++i) {
        const std::size_t entry = static_cast<std::size_t>(header_.external_ref_offset) +
                                  static_cast<std::size_t>(i) * kExternalRefEntrySize;
        BinaryReader reader(data_.subspan(entry, kExternalRefEntrySize));
        PARCEL_TRY_DECL(core::Guid128, guid, reader.ReadGuid128());
        PARCEL_TRY_DECL(std::uint32_t, flags, reader.ReadU32());
        if (guid.IsZero()) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "asset external reference GUID is zero");
        }
        refs.push_back(AssetExternalRefView{guid, flags});
    }
    external_refs_ = std::move(refs);
    return {};
}

Result<void> AssetFileReader::ReadSubAssets() {
    if (!header_read_) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "read the asset header first");
    }
    std::vector<AssetSubAssetView> sub_assets;
    sub_assets.reserve(header_.sub_asset_count);
    std::unordered_set<std::string_view> names;
    for (std::uint32_t i = 0; i < header_.sub_asset_count; ++i) {
        const std::size_t entry = static_cast<std::size_t>(header_.sub_asset_offset) +
                                  static_cast<std::size_t>(i) * kSubAssetEntrySize;
        BinaryReader reader(data_.subspan(entry, kSubAssetEntrySize));
        PARCEL_TRY_DECL(std::uint32_t, name_offset, reader.ReadU32());
        PARCEL_TRY_DECL(std::uint32_t, local_object_id, reader.ReadU32());
        PARCEL_TRY_DECL(std::uint32_t, flags, reader.ReadU32());
        if (name_offset >= header_.file_size) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "sub-asset name offset is invalid");
        }
        auto name = BinaryReader(data_.subspan(name_offset)).ReadCString();
        if (!name) {
            return std::move(name).TakeError();
        }
        if (name.Value().empty() || !names.insert(name.Value()).second) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "sub-asset name is empty or duplicated");
        }
        sub_assets.push_back(AssetSubAssetView{name.Value(), local_object_id, flags});
    }
    sub_assets_ = std::move(sub_assets);
    return {};
}

Result<void> AssetFileReader::ReadBlobs() {
    if (!header_read_) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "read the asset header first");
    }
    std::vector<AssetBlobView> blobs;
    blobs.reserve(header_.blob_count);
    std::vector<ByteRange> ranges;
    ranges.push_back(ByteRange{0, header_.header_size});
    ranges.push_back(ByteRange{header_.type_table_offset,
                               static_cast<std::uint64_t>(header_.type_count) * kTypeEntrySize});
    ranges.push_back(ByteRange{header_.object_table_offset,
                               static_cast<std::uint64_t>(header_.object_count) *
                                   kObjectEntrySize});
    ranges.push_back(ByteRange{header_.external_ref_offset,
                               static_cast<std::uint64_t>(header_.external_ref_count) *
                                   kExternalRefEntrySize});
    ranges.push_back(ByteRange{header_.sub_asset_offset,
                               static_cast<std::uint64_t>(header_.sub_asset_count) *
                                   kSubAssetEntrySize});
    ranges.push_back(ByteRange{header_.flatbuffer_offset, header_.flatbuffer_size});
    ranges.push_back(ByteRange{header_.blob_table_offset,
                               static_cast<std::uint64_t>(header_.blob_count) * kBlobEntrySize});

    for (std::uint32_t i = 0; i < header_.blob_count; ++i) {
        const std::size_t entry = static_cast<std::size_t>(header_.blob_table_offset) +
                                  static_cast<std::size_t>(i) * kBlobEntrySize;
        BinaryReader reader(data_.subspan(entry, kBlobEntrySize));
        PARCEL_TRY_DECL(std::uint64_t, offset, reader.ReadU64());
        PARCEL_TRY_DECL(std::uint64_t, size, reader.ReadU64());
        PARCEL_TRY_DECL(std::uint32_t, crc, reader.ReadU32());
        PARCEL_TRY_DECL(std::uint8_t, codec_value, reader.ReadU8());
        PARCEL_TRY_DECL(std::uint8_t, flags, reader.ReadU8());
        PARCEL_TRY(reader.Skip(2));
        if (size > std::numeric_limits<std::size_t>::max()) {
            return core::MakeError(core::ErrorCode::Corrupt, "asset blob is too large");
        }
        auto range = CheckRange(header_.file_size, offset, size, 1, "asset blob data");
        if (!range) {
            return std::move(range).TakeError();
        }
        auto codec = ParseCompressionCodec(codec_value);
        if (!codec) {
            return std::move(codec).TakeError();
        }
        if (codec.Value() != CompressionCodec::None) {
            return core::MakeError(core::ErrorCode::Unsupported,
                                   "compressed PRCA blobs are not supported");
        }
        const auto bytes = data_.subspan(static_cast<std::size_t>(offset),
                                         static_cast<std::size_t>(size));
        if (core::Crc32(bytes) != crc) {
            return core::MakeError(core::ErrorCode::HashMismatch, "asset blob CRC mismatch");
        }
        ranges.push_back(ByteRange{offset, size});
        blobs.push_back(AssetBlobView{bytes, crc, codec.Value(), flags});
    }
    PARCEL_TRY(CheckNoOverlap(ranges, "asset blob"));
    blobs_ = std::move(blobs);
    return {};
}

Result<void> AssetFileReader::ValidateReferences() const {
    std::unordered_set<std::uint32_t> type_ids;
    for (const auto& type : types_) {
        type_ids.insert(type.type_id);
    }
    if (type_ids.empty() && !objects_.empty()) {
        return core::MakeError(core::ErrorCode::Corrupt, "asset objects have no type table");
    }
    std::unordered_set<std::uint32_t> local_ids;
    for (const auto& object : objects_) {
        if (!type_ids.empty() && type_ids.find(object.type_id) == type_ids.end()) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "asset object references an unknown type");
        }
        if (!blobs_.empty() && object.blob_id != kNoBlob && object.blob_id >= blobs_.size()) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "asset object references an unknown blob");
        }
        local_ids.insert(object.local_id);
    }
    for (const auto& sub_asset : sub_assets_) {
        if (!local_ids.empty() && local_ids.find(sub_asset.local_object_id) == local_ids.end()) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "sub-asset references an unknown local object");
        }
    }
    return {};
}

Result<void> AssetFileReader::ReadAll(bool verify_content_hash) {
    PARCEL_TRY(ReadHeader());
    PARCEL_TRY(ReadTypes());
    PARCEL_TRY(ReadObjects());
    PARCEL_TRY(ReadExternalRefs());
    PARCEL_TRY(ReadSubAssets());
    PARCEL_TRY(ReadBlobs());
    PARCEL_TRY(ValidateReferences());
    if (verify_content_hash) {
        PARCEL_TRY(VerifyContentHash());
    }
    return {};
}

Result<void> AssetFileReader::VerifyContentHash() const {
    if (!header_read_) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "read the asset header first");
    }
    const core::Hash256 actual =
        core::Sha256(data_.subspan(header_.header_size));
    if (actual != header_.content_hash) {
        return core::MakeError(core::ErrorCode::HashMismatch,
                               "asset content hash mismatch");
    }
    return {};
}

Result<AssetFileReader> AssetFileReader::Open(std::span<const std::byte> data,
                                              bool verify_content_hash) {
    AssetFileReader reader(data);
    PARCEL_TRY(reader.ReadAll(verify_content_hash));
    return reader;
}

} // namespace parcel::serialize
