#include "parcel/serialize/builtin_catalog.hpp"

#include "parcel/serialize/binary.hpp"

#include <limits>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>

namespace parcel::serialize {
namespace {

using core::ErrorCode;
using core::MakeError;

Result<void> WriteString16(BinaryWriter& writer, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
        return MakeError(ErrorCode::InvalidArgument, "catalog string is too long");
    }
    PARCEL_TRY(writer.WriteU16(static_cast<std::uint16_t>(value.size())));
    return writer.WriteBytes(std::as_bytes(std::span(value.data(), value.size())));
}

Result<std::string> ReadString16(BinaryReader& reader) {
    PARCEL_TRY_DECL(std::uint16_t, length, reader.ReadU16());
    auto bytes = reader.ReadBytes(length);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    return std::string(reinterpret_cast<const char*>(bytes.Value().data()), length);
}

} // namespace

Result<std::vector<std::byte>> EncodeBuiltinCatalog(const BuiltinCatalog& catalog) {
    if (catalog.package_name.empty() || catalog.package_version.empty()) {
        return MakeError(ErrorCode::InvalidArgument,
                         "builtin catalog package name and version are required");
    }
    if (catalog.entries.size() >
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return MakeError(ErrorCode::InvalidArgument, "builtin catalog has too many entries");
    }

    BinaryWriter writer(64 + catalog.entries.size() * 48);
    PARCEL_TRY(writer.WriteU32(kBuiltinCatalogMagic));
    PARCEL_TRY(writer.WriteI32(kBuiltinCatalogVersion));
    PARCEL_TRY(WriteString16(writer, catalog.package_name));
    PARCEL_TRY(WriteString16(writer, catalog.package_version));
    PARCEL_TRY(writer.WriteI32(static_cast<std::int32_t>(catalog.entries.size())));
    std::unordered_set<std::string> file_names;
    std::unordered_set<std::string> guids;
    for (const BuiltinCatalogEntry& entry : catalog.entries) {
        if (entry.bundle_guid.IsZero() || entry.file_name.empty()) {
            return MakeError(ErrorCode::InvalidArgument,
                             "builtin catalog entry requires a GUID and file name");
        }
        if (!file_names.insert(entry.file_name).second ||
            !guids.insert(entry.bundle_guid.ToHex()).second) {
            return MakeError(ErrorCode::InvalidArgument,
                             "builtin catalog contains a duplicate entry");
        }
        PARCEL_TRY(WriteString16(writer, entry.bundle_guid.ToHex()));
        PARCEL_TRY(WriteString16(writer, entry.file_name));
    }
    if (writer.Size() > kBuiltinCatalogMaxBytes) {
        return MakeError(ErrorCode::InvalidArgument, "builtin catalog exceeds the size limit");
    }
    return writer.Release();
}

Result<BuiltinCatalog> DecodeBuiltinCatalog(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > kBuiltinCatalogMaxBytes) {
        return MakeError(ErrorCode::Corrupt, "builtin catalog has an invalid size");
    }

    BinaryReader reader(bytes);
    PARCEL_TRY_DECL(std::uint32_t, magic, reader.ReadU32());
    if (magic != kBuiltinCatalogMagic) {
        return MakeError(ErrorCode::Corrupt, "builtin catalog has an invalid magic");
    }
    PARCEL_TRY_DECL(std::int32_t, version, reader.ReadI32());
    if (version != kBuiltinCatalogVersion) {
        return MakeError(ErrorCode::Unsupported, "builtin catalog version is not supported");
    }

    BuiltinCatalog catalog;
    PARCEL_TRY_ASSIGN(catalog.package_name, ReadString16(reader));
    PARCEL_TRY_ASSIGN(catalog.package_version, ReadString16(reader));
    if (catalog.package_name.empty() || catalog.package_version.empty()) {
        return MakeError(ErrorCode::Corrupt, "builtin catalog package name or version is empty");
    }

    PARCEL_TRY_DECL(std::int32_t, count, reader.ReadI32());
    if (count < 0 || static_cast<std::size_t>(count) > reader.Remaining() / 4) {
        return MakeError(ErrorCode::Corrupt, "builtin catalog entry count is invalid");
    }
    catalog.entries.reserve(static_cast<std::size_t>(count));

    std::unordered_set<std::string> file_names;
    std::unordered_set<std::string> guids;
    for (std::int32_t index = 0; index < count; ++index) {
        PARCEL_TRY_DECL(std::string, guid_text, ReadString16(reader));
        PARCEL_TRY_DECL(std::string, file_name, ReadString16(reader));
        if (file_name.empty()) {
            return MakeError(ErrorCode::Corrupt, "builtin catalog entry has an empty file name");
        }
        PARCEL_TRY_DECL(core::AssetGuid, guid, core::AssetGuid::FromHex(guid_text));
        if (guid.IsZero()) {
            return MakeError(ErrorCode::Corrupt, "builtin catalog entry has a zero GUID");
        }
        if (!file_names.insert(file_name).second || !guids.insert(guid.ToHex()).second) {
            return MakeError(ErrorCode::Corrupt, "builtin catalog contains a duplicate entry");
        }
        BuiltinCatalogEntry entry;
        entry.bundle_guid = guid;
        entry.file_name = std::move(file_name);
        catalog.entries.push_back(std::move(entry));
    }

    if (!reader.IsAtEnd()) {
        return MakeError(ErrorCode::Corrupt, "builtin catalog has trailing bytes");
    }
    return catalog;
}

} // namespace parcel::serialize
