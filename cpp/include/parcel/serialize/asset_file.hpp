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

struct AssetHeader {
    std::uint16_t version = kAssetVersion;
    std::uint16_t header_size = kAssetHeaderSize;
    std::uint32_t flags = 0;
    std::uint64_t file_size = 0;
    std::uint64_t type_table_offset = 0;
    std::uint32_t type_count = 0;
    std::uint64_t object_table_offset = 0;
    std::uint32_t object_count = 0;
    std::uint64_t external_ref_offset = 0;
    std::uint32_t external_ref_count = 0;
    std::uint64_t sub_asset_offset = 0;
    std::uint32_t sub_asset_count = 0;
    std::uint64_t flatbuffer_offset = 0;
    std::uint32_t flatbuffer_size = 0;
    std::uint64_t blob_table_offset = 0;
    std::uint32_t blob_count = 0;
    core::Hash256 content_hash;
};

struct AssetTypeDesc {
    std::uint32_t type_id = 0;
    std::string type_name;
    std::uint32_t serializer_version = 0;
    std::uint32_t flags = 0;
};

struct AssetObjectDesc {
    std::uint32_t local_id = 0;
    std::uint32_t type_id = 0;
    std::vector<std::byte> metadata;
    std::uint64_t blob_id = kNoBlob;
    std::uint32_t flags = 0;
};

struct AssetExternalRefDesc {
    core::AssetGuid asset_guid;
    std::uint32_t flags = 0;
};

struct AssetSubAssetDesc {
    std::string name;
    std::uint32_t local_object_id = 0;
    std::uint32_t flags = 0;
};

struct AssetBlobDesc {
    std::vector<std::byte> data;
    std::uint32_t flags = 0;
};

struct AssetFileDesc {
    std::vector<AssetTypeDesc> types;
    std::vector<AssetObjectDesc> objects;
    std::vector<AssetExternalRefDesc> external_refs;
    std::vector<AssetSubAssetDesc> sub_assets;
    std::vector<AssetBlobDesc> blobs;
};

class AssetFileWriter {
  public:
    AssetFileWriter() = default;

    void AddType(AssetTypeDesc type);
    void AddObject(AssetObjectDesc object);
    void AddExternalRef(AssetExternalRefDesc reference);
    void AddSubAsset(AssetSubAssetDesc sub_asset);
    void AddBlob(AssetBlobDesc blob);

    [[nodiscard]] const AssetFileDesc& Description() const noexcept { return desc_; }
    [[nodiscard]] Result<std::vector<std::byte>> Build() const;

  private:
    AssetFileDesc desc_;
};

[[nodiscard]] Result<std::vector<std::byte>> WriteAssetFile(const AssetFileDesc& desc);

struct AssetTypeView {
    std::uint32_t type_id = 0;
    std::string_view type_name;
    std::uint32_t serializer_version = 0;
    std::uint32_t flags = 0;
};

struct AssetObjectView {
    std::uint32_t local_id = 0;
    std::uint32_t type_id = 0;
    std::span<const std::byte> metadata;
    std::uint64_t blob_id = kNoBlob;
    std::uint32_t flags = 0;
};

struct AssetExternalRefView {
    core::AssetGuid asset_guid;
    std::uint32_t flags = 0;
};

struct AssetSubAssetView {
    std::string_view name;
    std::uint32_t local_object_id = 0;
    std::uint32_t flags = 0;
};

struct AssetBlobView {
    std::span<const std::byte> data;
    std::uint32_t crc32 = 0;
    CompressionCodec codec = CompressionCodec::None;
    std::uint8_t flags = 0;
};

class AssetFileReader {
  public:
    explicit AssetFileReader(std::span<const std::byte> data) noexcept;

    static Result<AssetFileReader> Open(std::span<const std::byte> data,
                                        bool verify_content_hash = true);

    [[nodiscard]] Result<void> ReadHeader();
    [[nodiscard]] Result<void> ReadTypes();
    [[nodiscard]] Result<void> ReadObjects();
    [[nodiscard]] Result<void> ReadExternalRefs();
    [[nodiscard]] Result<void> ReadSubAssets();
    [[nodiscard]] Result<void> ReadBlobs();
    [[nodiscard]] Result<void> ReadAll(bool verify_content_hash = true);
    [[nodiscard]] Result<void> VerifyContentHash() const;

    [[nodiscard]] const AssetHeader& Header() const noexcept { return header_; }
    [[nodiscard]] std::span<const AssetTypeView> Types() const noexcept { return types_; }
    [[nodiscard]] std::span<const AssetObjectView> Objects() const noexcept { return objects_; }
    [[nodiscard]] std::span<const AssetExternalRefView> ExternalRefs() const noexcept {
        return external_refs_;
    }
    [[nodiscard]] std::span<const AssetSubAssetView> SubAssets() const noexcept {
        return sub_assets_;
    }
    [[nodiscard]] std::span<const AssetBlobView> Blobs() const noexcept { return blobs_; }

  private:
    [[nodiscard]] Result<void> ValidateReferences() const;

    std::span<const std::byte> data_;
    AssetHeader header_;
    bool header_read_ = false;
    std::vector<AssetTypeView> types_;
    std::vector<AssetObjectView> objects_;
    std::vector<AssetExternalRefView> external_refs_;
    std::vector<AssetSubAssetView> sub_assets_;
    std::vector<AssetBlobView> blobs_;
};

} // namespace parcel::serialize
