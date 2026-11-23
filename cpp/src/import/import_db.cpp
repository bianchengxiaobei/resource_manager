#include "parcel/import/import_db.hpp"

#include "parcel/core/error.hpp"
#include "parcel/core/path.hpp"
#include "parcel/core/platform_file.hpp"
#include "parcel/serialize/binary.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace parcel::import {
namespace {

using core::ErrorCode;
using core::MakeError;

constexpr std::array<std::byte, 4> kMagic = {std::byte{'P'}, std::byte{'R'}, std::byte{'I'},
                                             std::byte{'D'}};
constexpr std::uint16_t kHeaderSize = 64;
constexpr std::uint32_t kMaxRecords = 10U * 1000U * 1000U;
constexpr std::uint32_t kMaxEntries = 10U * 1000U * 1000U;

core::Result<std::string> ParentDirectory(std::string_view path) {
    const std::string normalized = core::path::Normalize(path);
    const auto slash = normalized.find_last_of('/');
    if (slash == std::string::npos || slash == 0 || (slash == 2 && normalized[1] == ':')) {
        return std::string{};
    }
    return normalized.substr(0, slash);
}

core::Result<void> SaveAtomic(std::string_view path, std::span<const std::byte> data) {
    core::PlatformFile platform;
    if (auto parent = ParentDirectory(path); parent) {
        if (!parent.Value().empty()) {
            PARCEL_TRY(platform.CreateDirectories(parent.Value()));
        }
    } else {
        return std::move(parent).TakeError();
    }

    const std::string normalized = core::path::Normalize(path);
    const std::string temporary = normalized + ".tmp";
    PARCEL_TRY_DECL(std::unique_ptr<core::IFile>, file, platform.OpenWrite(temporary));
    PARCEL_TRY(core::WriteAll(*file, data));
    PARCEL_TRY(file->Flush());
    file->Close();
    return platform.Rename(temporary, normalized, true);
}

void SortRecord(ImportDBRecord& record) {
    std::sort(
        record.outputs.begin(), record.outputs.end(),
        [](const ImportDBOutput& lhs, const ImportDBOutput& rhs) { return lhs.guid < rhs.guid; });
    std::sort(record.dependencies.begin(), record.dependencies.end(),
              [](const ImportDBDependency& lhs, const ImportDBDependency& rhs) {
                  return lhs.guid < rhs.guid;
              });
}

} // namespace

const ImportDBRecord* ImportDB::FindSource(std::string_view source_path) const noexcept {
    const auto found =
        std::find_if(records.begin(), records.end(), [&](const ImportDBRecord& record) {
            return record.source_path == source_path;
        });
    return found == records.end() ? nullptr : &*found;
}

const ImportDBOutput* ImportDB::FindOutput(core::AssetGuid guid) const noexcept {
    for (const ImportDBRecord& record : records) {
        const auto found =
            std::find_if(record.outputs.begin(), record.outputs.end(),
                         [&](const ImportDBOutput& output) { return output.guid == guid; });
        if (found != record.outputs.end()) {
            return &*found;
        }
    }
    return nullptr;
}

core::Result<ImportDB> LoadImportDB(std::string_view path) {
    core::PlatformFile platform;
    auto file = platform.OpenRead(path);
    if (!file) {
        auto error = std::move(file).TakeError();
        if (error.code == ErrorCode::NotFound) {
            return ImportDB{};
        }
        return error;
    }
    auto bytes = core::ReadAll(*file.Value());
    if (!bytes) {
        return std::move(bytes).TakeError();
    }

    serialize::BinaryReader reader(bytes.Value());
    PARCEL_TRY_DECL(std::span<const std::byte>, magic, reader.ReadBytes(kMagic.size()));
    if (!std::equal(kMagic.begin(), kMagic.end(), magic.begin())) {
        return MakeError(ErrorCode::Corrupt, "import DB magic is invalid");
    }
    PARCEL_TRY_DECL(std::uint16_t, version, reader.ReadU16());
    if (version != kImportDBVersion) {
        return MakeError(ErrorCode::Unsupported, "import DB version is unsupported");
    }
    PARCEL_TRY_DECL(std::uint16_t, header_size, reader.ReadU16());
    if (header_size < 16 || header_size > 4096) {
        return MakeError(ErrorCode::Corrupt, "import DB header size is invalid");
    }
    PARCEL_TRY_DECL(std::uint32_t, record_count, reader.ReadU32());
    if (record_count > kMaxRecords) {
        return MakeError(ErrorCode::Corrupt, "import DB has too many records");
    }
    PARCEL_TRY_DECL(std::uint32_t, reserved, reader.ReadU32());
    if (reserved != 0) {
        return MakeError(ErrorCode::Corrupt, "import DB reserved field is nonzero");
    }
    if (header_size > 16) {
        PARCEL_TRY(reader.Skip(header_size - 16));
    }

    ImportDB db;
    db.records.reserve(record_count);
    for (std::uint32_t i = 0; i < record_count; ++i) {
        ImportDBRecord record;
        PARCEL_TRY_DECL(std::string_view, source_path, reader.ReadString());
        record.source_path.assign(source_path);
        PARCEL_TRY_ASSIGN(record.source_guid, reader.ReadGuid128());
        PARCEL_TRY_DECL(std::string_view, importer, reader.ReadString());
        record.importer.assign(importer);
        PARCEL_TRY_ASSIGN(record.importer_version, reader.ReadU32());
        PARCEL_TRY_ASSIGN(record.source_hash, reader.ReadHash256());
        PARCEL_TRY_ASSIGN(record.settings_hash, reader.ReadHash256());
        PARCEL_TRY_ASSIGN(record.texture_index_hash, reader.ReadHash256());

        PARCEL_TRY_DECL(std::uint32_t, output_count, reader.ReadU32());
        if (output_count > kMaxEntries) {
            return MakeError(ErrorCode::Corrupt, "import DB has too many outputs");
        }
        record.outputs.reserve(output_count);
        for (std::uint32_t j = 0; j < output_count; ++j) {
            ImportDBOutput output;
            PARCEL_TRY_ASSIGN(output.guid, reader.ReadGuid128());
            PARCEL_TRY_ASSIGN(output.output_hash, reader.ReadHash256());
            record.outputs.push_back(output);
        }

        PARCEL_TRY_DECL(std::uint32_t, dependency_count, reader.ReadU32());
        if (dependency_count > kMaxEntries) {
            return MakeError(ErrorCode::Corrupt, "import DB has too many dependencies");
        }
        record.dependencies.reserve(dependency_count);
        for (std::uint32_t j = 0; j < dependency_count; ++j) {
            ImportDBDependency dependency;
            PARCEL_TRY_ASSIGN(dependency.guid, reader.ReadGuid128());
            PARCEL_TRY_ASSIGN(dependency.output_hash, reader.ReadHash256());
            record.dependencies.push_back(dependency);
        }
        db.records.push_back(std::move(record));
    }
    if (!reader.IsAtEnd()) {
        return MakeError(ErrorCode::Corrupt, "import DB has trailing bytes");
    }
    return db;
}

core::Result<void> SaveImportDB(std::string_view path, const ImportDB& db) {
    return SaveImportDB(path, db.records);
}

core::Result<void> SaveImportDB(std::string_view path, const std::vector<ImportDBRecord>& records) {
    if (path.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "import DB path is empty");
    }
    if (records.size() > kMaxRecords) {
        return MakeError(ErrorCode::InvalidArgument, "import DB has too many records");
    }

    std::vector<ImportDBRecord> sorted = records;
    for (ImportDBRecord& record : sorted) {
        SortRecord(record);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const ImportDBRecord& lhs, const ImportDBRecord& rhs) {
                  if (lhs.source_path != rhs.source_path) {
                      return lhs.source_path < rhs.source_path;
                  }
                  return lhs.source_guid < rhs.source_guid;
              });

    serialize::BinaryWriter writer(64 + sorted.size() * 256);
    PARCEL_TRY(writer.WriteBytes(kMagic));
    PARCEL_TRY(writer.WriteU16(kImportDBVersion));
    PARCEL_TRY(writer.WriteU16(kHeaderSize));
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(sorted.size())));
    PARCEL_TRY(writer.WriteU32(0));
    PARCEL_TRY(writer.WriteZero(kHeaderSize - 16));

    for (const ImportDBRecord& record : sorted) {
        PARCEL_TRY(writer.WriteString(record.source_path));
        PARCEL_TRY(writer.WriteGuid128(record.source_guid));
        PARCEL_TRY(writer.WriteString(record.importer));
        PARCEL_TRY(writer.WriteU32(record.importer_version));
        PARCEL_TRY(writer.WriteHash256(record.source_hash));
        PARCEL_TRY(writer.WriteHash256(record.settings_hash));
        PARCEL_TRY(writer.WriteHash256(record.texture_index_hash));
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(record.outputs.size())));
        for (const ImportDBOutput& output : record.outputs) {
            PARCEL_TRY(writer.WriteGuid128(output.guid));
            PARCEL_TRY(writer.WriteHash256(output.output_hash));
        }
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(record.dependencies.size())));
        for (const ImportDBDependency& dependency : record.dependencies) {
            PARCEL_TRY(writer.WriteGuid128(dependency.guid));
            PARCEL_TRY(writer.WriteHash256(dependency.output_hash));
        }
    }

    return SaveAtomic(path, writer.Data());
}

} // namespace parcel::import
