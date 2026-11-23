#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/serialize/binary.hpp"

#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace parcel::assets::detail {

using core::ErrorCode;
using core::MakeError;
using serialize::BinaryReader;
using serialize::BinaryWriter;

inline Result<void> Finish(BinaryReader& reader, const char* what) {
    if (!reader.IsAtEnd()) {
        return MakeError(ErrorCode::Corrupt, std::string(what) + " has trailing bytes");
    }
    return {};
}

inline Result<void> WriteGuid(BinaryWriter& writer, const AssetGuid& guid) {
    return writer.WriteGuid128(guid);
}

inline Result<AssetGuid> ReadGuid(BinaryReader& reader) {
    AssetGuid guid;
    PARCEL_TRY_ASSIGN(guid, reader.ReadGuid128());
    return guid;
}

inline Result<std::uint64_t> WriteBlob(IAssetWriteContext& context,
                                       std::span<const std::byte> data) {
    return context.AddBlob(data);
}

inline Result<std::span<const std::byte>> ReadBlob(IAssetReadContext& context, std::uint64_t id) {
    return context.Blob(id);
}

template <typename T> Result<T> ReadEnum(BinaryReader& reader, std::uint8_t maximum) {
    PARCEL_TRY_DECL(std::uint8_t, raw, reader.ReadU8());
    if (raw > maximum) {
        return MakeError(ErrorCode::Corrupt, "asset enum value is out of range");
    }
    return static_cast<T>(raw);
}

template <typename T> Result<void> WriteEnum(BinaryWriter& writer, T value) {
    return writer.WriteU8(static_cast<std::uint8_t>(value));
}

inline Result<std::uint32_t> ReadCount(BinaryReader& reader, std::uint32_t maximum,
                                       std::uint32_t minimum_entry_size, const char* what) {
    PARCEL_TRY_DECL(std::uint32_t, count, reader.ReadU32());
    if (count > maximum) {
        return MakeError(ErrorCode::Corrupt, std::string(what) + " count exceeds limit");
    }
    if (minimum_entry_size != 0 &&
        static_cast<std::uint64_t>(count) * minimum_entry_size > reader.Remaining()) {
        return MakeError(ErrorCode::Corrupt, std::string(what) + " count exceeds metadata");
    }
    return count;
}

} // namespace parcel::assets::detail
