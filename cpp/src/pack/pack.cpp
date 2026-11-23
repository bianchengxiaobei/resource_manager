#include "parcel/pack/pack.hpp"

#include "parcel/assets/asset_codec.hpp"
#include "parcel/assets/json.hpp"
#include "parcel/assets/serializers.hpp"
#include "parcel/assets/shader_library_asset.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/error.hpp"
#include "parcel/core/path.hpp"
#include "parcel/core/platform_file.hpp"
#include "parcel/serialize/asset_file.hpp"
#include "parcel/serialize/builtin_catalog.hpp"
#include "parcel/serialize/bundle.hpp"
#include "parcel/serialize/manifest.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace parcel::pack {
namespace {

using core::ErrorCode;
using core::MakeError;
using core::Result;

constexpr std::string_view kPackToolVersion = "0.4.0";
constexpr std::uint32_t kBuildDBVersion = 1;

std::filesystem::path Utf8Path(std::string_view path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

std::string FromPath(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string NormalizePath(std::string_view value) { return core::path::Normalize(value); }

Result<std::string> AbsolutePath(std::string_view path) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(Utf8Path(path), error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to resolve absolute path: " + error.message())
            .WithPath(std::string(path));
    }
    return NormalizePath(FromPath(absolute));
}

Result<std::string> RelativeTo(std::string_view root, std::string_view path) {
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(Utf8Path(path), Utf8Path(root), error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to make path relative: " + error.message())
            .WithPath(std::string(path));
    }
    std::string result = NormalizePath(FromPath(relative));
    if (result == ".." || result.rfind("../", 0) == 0) {
        return MakeError(ErrorCode::InvalidArgument, "path escapes its root")
            .WithPath(std::string(path));
    }
    return result;
}

std::string DirectoryOf(std::string_view path) {
    const std::string normalized = NormalizePath(path);
    const std::size_t slash = normalized.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    if (slash == 0) {
        return "/";
    }
    if (slash == 2 && normalized[1] == ':') {
        return normalized.substr(0, 2);
    }
    return normalized.substr(0, slash);
}

std::string FileNameOf(std::string_view path) {
    return std::string(core::path::FileName(NormalizePath(path)));
}

std::string StemOf(std::string_view path) {
    return std::string(core::path::Stem(NormalizePath(path)));
}

std::string LowerAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool HasNul(std::string_view value) { return value.find('\0') != std::string_view::npos; }

bool HasControl(std::string_view value) {
    return std::any_of(value.begin(), value.end(),
                       [](unsigned char c) { return c < 0x20U && c != '\t'; });
}

bool IsSafeBundleName(std::string_view value) {
    return !value.empty() && !HasNul(value) && !HasControl(value) &&
           core::path::IsSafeRelative(value);
}

std::string JoinName(std::string_view left, std::string_view right) {
    if (left.empty()) {
        return std::string(right);
    }
    if (right.empty()) {
        return std::string(left);
    }
    return std::string(left) + "/" + std::string(right);
}

std::string ParentBundleName(std::string_view asset_path) {
    const std::string parent = DirectoryOf(asset_path);
    if (parent.empty() || parent == "/" || parent == "//") {
        return "root";
    }
    return parent;
}

Result<void> EnsureDirectory(std::string_view path) {
    if (path.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "directory path is empty");
    }
    core::PlatformFile platform;
    PARCEL_TRY(platform.CreateDirectories(path));
    return {};
}

Result<std::vector<std::byte>> ReadFileBytes(std::string_view path) {
    core::PlatformFile platform;
    PARCEL_TRY_DECL(std::unique_ptr<core::IFile>, file, platform.OpenRead(path));
    auto bytes = core::ReadAll(*file);
    file->Close();
    return bytes;
}

Result<void> WriteBytesAtomic(std::string_view path, std::span<const std::byte> bytes) {
    core::PlatformFile platform;
    const std::string normalized = NormalizePath(path);
    const std::string temporary = normalized + ".tmp";
    PARCEL_TRY_DECL(std::unique_ptr<core::IFile>, file, platform.OpenWrite(temporary));
    PARCEL_TRY(core::WriteAll(*file, bytes));
    PARCEL_TRY(file->Flush());
    file->Close();
    return platform.Rename(temporary, normalized, true);
}

Result<void> WriteTextFile(std::string_view path, std::string_view text) {
    PARCEL_TRY(
        WriteBytesAtomic(path, std::as_bytes(std::span<const char>(text.data(), text.size()))));
    return {};
}

void AddString(assets::JsonValue& object, std::string key, std::string value) {
    assets::JsonValue field;
    field.type = assets::JsonType::String;
    field.string_value = std::move(value);
    object.object_value.emplace_back(std::move(key), std::move(field));
}

void AddNumber(assets::JsonValue& object, std::string key, double value) {
    assets::JsonValue field;
    field.type = assets::JsonType::Number;
    field.number_value = value;
    object.object_value.emplace_back(std::move(key), std::move(field));
}

void AddBool(assets::JsonValue& object, std::string key, bool value) {
    assets::JsonValue field;
    field.type = assets::JsonType::Bool;
    field.bool_value = value;
    object.object_value.emplace_back(std::move(key), std::move(field));
}

assets::JsonValue StringArray(const std::vector<std::string>& values) {
    assets::JsonValue array;
    array.type = assets::JsonType::Array;
    for (const std::string& value : values) {
        assets::JsonValue item;
        item.type = assets::JsonType::String;
        item.string_value = value;
        array.array_value.push_back(std::move(item));
    }
    return array;
}

const assets::JsonValue* FindField(const assets::JsonValue& object, std::string_view key) {
    if (!object.IsObject()) {
        return nullptr;
    }
    return object.Find(key);
}

Result<std::string> ReadString(const assets::JsonValue& object, std::string_view key,
                               std::string fallback = {}, bool required = false) {
    const assets::JsonValue* field = FindField(object, key);
    if (field == nullptr) {
        if (required) {
            return MakeError(ErrorCode::InvalidArgument,
                             "missing required config field '" + std::string(key) + "'");
        }
        return fallback;
    }
    if (!field->IsString()) {
        return MakeError(ErrorCode::InvalidArgument,
                         "config field '" + std::string(key) + "' must be a string");
    }
    return field->string_value;
}

Result<bool> ReadBool(const assets::JsonValue& object, std::string_view key, bool fallback) {
    const assets::JsonValue* field = FindField(object, key);
    if (field == nullptr) {
        return fallback;
    }
    if (!field->IsBool()) {
        return MakeError(ErrorCode::InvalidArgument,
                         "config field '" + std::string(key) + "' must be a boolean");
    }
    return field->bool_value;
}

Result<std::uint32_t> ReadUInt(const assets::JsonValue& object, std::string_view key,
                               std::uint32_t fallback) {
    const assets::JsonValue* field = FindField(object, key);
    if (field == nullptr) {
        return fallback;
    }
    if (!field->IsNumber() || field->number_value < 0.0 ||
        field->number_value > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        field->number_value !=
            static_cast<double>(static_cast<std::uint32_t>(field->number_value))) {
        return MakeError(ErrorCode::InvalidArgument,
                         "config field '" + std::string(key) + "' must be an unsigned integer");
    }
    return static_cast<std::uint32_t>(field->number_value);
}

Result<std::vector<std::string>> ReadStringArray(const assets::JsonValue& object,
                                                 std::string_view key) {
    const assets::JsonValue* field = FindField(object, key);
    if (field == nullptr) {
        return std::vector<std::string>{};
    }
    if (!field->IsArray()) {
        return MakeError(ErrorCode::InvalidArgument,
                         "config field '" + std::string(key) + "' must be an array");
    }
    std::vector<std::string> result;
    result.reserve(field->array_value.size());
    for (const assets::JsonValue& item : field->array_value) {
        if (!item.IsString()) {
            return MakeError(ErrorCode::InvalidArgument,
                             "config field '" + std::string(key) + "' must contain strings");
        }
        result.push_back(item.string_value);
    }
    return result;
}

Result<CollectorType> ParseCollectorType(std::string_view value) {
    const std::string lower = LowerAscii(value);
    if (lower == "main" || lower == "mainassetcollector") {
        return CollectorType::Main;
    }
    if (lower == "static" || lower == "staticassetcollector") {
        return CollectorType::Static;
    }
    if (lower == "depend" || lower == "dependency" || lower == "dependassetcollector") {
        return CollectorType::Depend;
    }
    return MakeError(ErrorCode::InvalidArgument,
                     "unknown collector type '" + std::string(value) + "'");
}

Result<AddressRule> ParseAddressRule(std::string_view value) {
    const std::string lower = LowerAscii(value);
    if (lower == "assetpath" || lower == "path") {
        return AddressRule::AssetPath;
    }
    if (lower == "filename" || lower == "file") {
        return AddressRule::FileName;
    }
    if (lower == "guid") {
        return AddressRule::Guid;
    }
    if (lower == "prefix") {
        return AddressRule::Prefix;
    }
    if (lower == "address") {
        return AddressRule::Address;
    }
    return MakeError(ErrorCode::InvalidArgument,
                     "unknown address rule '" + std::string(value) + "'");
}

Result<serialize::CompressionCodec> ParseCompression(std::string_view value) {
    const std::string lower = LowerAscii(value);
    if (lower == "none") {
        return serialize::CompressionCodec::None;
    }
    if (lower == "lz4") {
        return serialize::CompressionCodec::Lz4;
    }
    if (lower == "zstd") {
        return serialize::CompressionCodec::Zstd;
    }
    return MakeError(ErrorCode::InvalidArgument,
                     "unknown compression codec '" + std::string(value) + "'");
}

int CollectorPriority(CollectorType type) {
    switch (type) {
    case CollectorType::Main:
        return 3;
    case CollectorType::Static:
        return 2;
    case CollectorType::Depend:
        return 1;
    }
    return 0;
}

} // namespace

Result<PackConfig> LoadPackConfig(std::string_view path) {
    auto bytes = ReadFileBytes(path);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    const std::string text(reinterpret_cast<const char*>(bytes.Value().data()),
                           bytes.Value().size());
    auto parsed = assets::ParseJson(text);
    if (!parsed) {
        return std::move(parsed).TakeError();
    }
    const assets::JsonValue& root = parsed.Value();
    if (!root.IsObject()) {
        return MakeError(ErrorCode::InvalidArgument, "pack config root must be an object");
    }

    PackConfig config;
    PARCEL_TRY_DECL(std::string, package_name, ReadString(root, "package"));
    PARCEL_TRY_DECL(std::string, package_version, ReadString(root, "version"));
    PARCEL_TRY_DECL(std::string, target, ReadString(root, "target", "windows"));
    PARCEL_TRY_DECL(std::string, cooked, ReadString(root, "cooked"));
    PARCEL_TRY_DECL(std::string, output, ReadString(root, "output"));
    config.package_name = std::move(package_name);
    config.package_version = std::move(package_version);
    config.target = std::move(target);
    config.cooked_root = std::move(cooked);
    config.output_root = std::move(output);

    if (const assets::JsonValue* compression = FindField(root, "compression");
        compression != nullptr) {
        if (!compression->IsString()) {
            return MakeError(ErrorCode::InvalidArgument,
                             "config field 'compression' must be a string");
        }
        auto parsed_codec = ParseCompression(compression->string_value);
        if (!parsed_codec) {
            return std::move(parsed_codec).TakeError();
        }
        config.compression = parsed_codec.Value();
    }
    PARCEL_TRY_ASSIGN(config.block_size, ReadUInt(root, "blockSize", config.block_size));
    PARCEL_TRY_DECL(
        std::uint32_t, compression_level,
        ReadUInt(root, "compressionLevel", static_cast<std::uint32_t>(config.compression_level)));
    config.compression_level = static_cast<int>(compression_level);
    PARCEL_TRY_ASSIGN(config.jobs, ReadUInt(root, "jobs", config.jobs));
    PARCEL_TRY_ASSIGN(config.enable_addressable,
                      ReadBool(root, "enableAddressable", config.enable_addressable));
    PARCEL_TRY_ASSIGN(config.support_extensionless,
                      ReadBool(root, "supportExtensionless", config.support_extensionless));
    PARCEL_TRY_ASSIGN(config.location_to_lower,
                      ReadBool(root, "locationToLower", config.location_to_lower));
    PARCEL_TRY_ASSIGN(config.include_asset_guid,
                      ReadBool(root, "includeAssetGuid", config.include_asset_guid));
    PARCEL_TRY_ASSIGN(
        config.replace_asset_path_with_address,
        ReadBool(root, "replaceAssetPathWithAddress", config.replace_asset_path_with_address));
    PARCEL_TRY_ASSIGN(config.auto_collect_shaders,
                      ReadBool(root, "autoCollectShaders", config.auto_collect_shaders));
    PARCEL_TRY_ASSIGN(config.write_builtin_catalog,
                      ReadBool(root, "writeBuiltinCatalog", config.write_builtin_catalog));
    PARCEL_TRY_ASSIGN(config.enable_share_bundle,
                      ReadBool(root, "enableShareBundle", config.enable_share_bundle));
    PARCEL_TRY_ASSIGN(
        config.single_referenced_pack_alone,
        ReadBool(root, "singleReferencedPackAlone", config.single_referenced_pack_alone));
    PARCEL_TRY_ASSIGN(config.clean_output, ReadBool(root, "clean", config.clean_output));
    PARCEL_TRY_ASSIGN(config.output_name_style,
                      ReadUInt(root, "outputNameStyle", config.output_name_style));
    PARCEL_TRY_ASSIGN(config.build_bundle_type,
                      ReadUInt(root, "buildBundleType", config.build_bundle_type));

    const assets::JsonValue* collectors = FindField(root, "collectors");
    if (collectors == nullptr) {
        CollectorConfig fallback;
        fallback.name = "main";
        fallback.type = CollectorType::Main;
        fallback.path = "";
        config.collectors.push_back(std::move(fallback));
        return config;
    }
    if (!collectors->IsArray()) {
        return MakeError(ErrorCode::InvalidArgument, "config field 'collectors' must be an array");
    }
    for (std::size_t index = 0; index < collectors->array_value.size(); ++index) {
        const assets::JsonValue& item = collectors->array_value[index];
        if (!item.IsObject()) {
            return MakeError(ErrorCode::InvalidArgument, "collector entry must be an object");
        }
        CollectorConfig collector;
        PARCEL_TRY_ASSIGN(collector.name,
                          ReadString(item, "name", "collector_" + std::to_string(index)));
        PARCEL_TRY_DECL(std::string, type_name, ReadString(item, "type", "main"));
        auto type = ParseCollectorType(type_name);
        if (!type) {
            return std::move(type).TakeError();
        }
        collector.type = type.Value();
        PARCEL_TRY_ASSIGN(collector.path, ReadString(item, "path"));
        PARCEL_TRY_ASSIGN(collector.files, ReadStringArray(item, "files"));
        PARCEL_TRY_ASSIGN(collector.recursive, ReadBool(item, "recursive", true));
        PARCEL_TRY_DECL(std::string, address_rule, ReadString(item, "addressRule", "assetPath"));
        auto rule = ParseAddressRule(address_rule);
        if (!rule) {
            return std::move(rule).TakeError();
        }
        collector.address_rule = rule.Value();
        PARCEL_TRY_ASSIGN(collector.address_prefix, ReadString(item, "addressPrefix"));
        PARCEL_TRY_ASSIGN(collector.address, ReadString(item, "address"));
        PARCEL_TRY_ASSIGN(collector.tags, ReadStringArray(item, "tags"));
        PARCEL_TRY_ASSIGN(collector.bundle, ReadString(item, "bundle"));
        PARCEL_TRY_ASSIGN(collector.share, ReadBool(item, "share", true));
        config.collectors.push_back(std::move(collector));
    }
    if (config.collectors.empty()) {
        CollectorConfig fallback;
        fallback.name = "main";
        fallback.type = CollectorType::Main;
        fallback.path = "";
        config.collectors.push_back(std::move(fallback));
    }
    return config;
}

namespace {

struct Selection {
    CollectorConfig collector;
    std::string relative_path;
};

struct AssetData {
    core::AssetGuid guid;
    std::string relative_path;
    std::string absolute_path;
    std::string type_name;
    CollectorType collector_type = CollectorType::Depend;
    std::string address_rule_value;
    AddressRule address_rule = AddressRule::AssetPath;
    std::string address_prefix;
    std::string explicit_address;
    std::string address;
    std::vector<std::string> tags;
    std::string bundle_hint;
    bool share = true;
    std::uint32_t local_object_id = 0;
    std::vector<core::AssetGuid> dependencies;
    std::vector<std::byte> bytes;
    core::Hash256 hash;
};

struct BuildDBBundle {
    std::string name;
    core::AssetGuid guid;
    std::string file_name;
    core::Hash256 file_hash;
    std::vector<std::pair<core::AssetGuid, core::Hash256>> members;
};

struct BuildDB {
    std::uint32_t version = kBuildDBVersion;
    std::string tool_version = std::string(kPackToolVersion);
    std::string package_name;
    std::string package_version;
    std::string target;
    std::vector<BuildDBBundle> bundles;
};

struct BundlePlan {
    std::string name;
    core::AssetGuid guid;
    std::string file_name;
    std::vector<core::AssetGuid> members;
    std::vector<std::string> tags;
    std::vector<std::string> dependency_names;
    std::vector<core::AssetGuid> dependency_guids;
    std::vector<core::AssetGuid> referrer_guids;
    std::vector<serialize::BundlePayloadDesc> payloads;
    std::vector<std::byte> bytes;
    std::uint64_t file_size = 0;
    core::Hash256 file_hash;
    std::uint32_t file_crc = 0;
    bool reused = false;
};

struct CookedFile {
    std::string relative_path;
    std::string absolute_path;
    core::AssetGuid guid;
};

struct PackContext {
    PackConfig config;
    std::string project_root;
    std::string cooked_root;
    std::string output_root;
    std::string output_directory;
    std::vector<CookedFile> cooked_files;
    std::map<core::AssetGuid, std::string> file_by_guid;
    std::map<core::AssetGuid, std::vector<Selection>> selections;
    std::map<core::AssetGuid, AssetData> assets;
    std::set<core::AssetGuid> roots;
    std::set<core::AssetGuid> reachable;
    std::map<core::AssetGuid, std::vector<core::AssetGuid>> referrers;
    std::map<core::AssetGuid, std::string> bundle_of;
    std::map<std::string, BundlePlan> bundles;
    std::map<core::AssetGuid, std::pair<std::string, std::uint32_t>> payload_of;
    BuildDB build_db;
    PackReport report;
    std::vector<std::string> warnings;
};

Result<std::string> ResolveConfiguredPath(const PackContext& context, std::string_view value) {
    if (value.empty()) {
        return std::string{};
    }
    if (core::path::IsAbsolute(value)) {
        return AbsolutePath(value);
    }
    auto joined = core::path::Join(context.project_root, value);
    if (!joined) {
        return std::move(joined).TakeError();
    }
    return AbsolutePath(joined.Value());
}

Result<std::string> MakeOutputDirectory(const PackContext& context) {
    if (context.config.package_name.empty() || context.config.package_version.empty() ||
        context.config.target.empty()) {
        return MakeError(ErrorCode::InvalidArgument,
                         "package name, version and target are required");
    }
    auto safe_component = [](std::string_view value) {
        return !value.empty() && !HasNul(value) && !HasControl(value) &&
               value.find('/') == std::string_view::npos &&
               value.find('\\') == std::string_view::npos && value != "." && value != ".." &&
               value.find(':') == std::string_view::npos;
    };
    if (!safe_component(context.config.package_name) ||
        !safe_component(context.config.package_version) || !safe_component(context.config.target)) {
        return MakeError(ErrorCode::InvalidArgument,
                         "package name, version and target must be safe path components");
    }
    auto target = core::path::Join(context.output_root, context.config.target);
    if (!target) {
        return std::move(target).TakeError();
    }
    auto package = core::path::Join(target.Value(), context.config.package_name);
    if (!package) {
        return std::move(package).TakeError();
    }
    auto version = core::path::Join(package.Value(), context.config.package_version);
    if (!version) {
        return std::move(version).TakeError();
    }
    return version;
}

Result<std::vector<CookedFile>> ScanCookedFiles(std::string_view root) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(Utf8Path(root), error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to scan cooked directory: " + error.message())
            .WithPath(std::string(root));
    }
    std::vector<CookedFile> result;
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            return MakeError(ErrorCode::Io, "failed to scan cooked directory: " + error.message())
                .WithPath(std::string(root));
        }
        if (!iterator->is_regular_file(error)) {
            if (error) {
                error.clear();
            }
            continue;
        }
        const std::string absolute = NormalizePath(FromPath(iterator->path()));
        if (LowerAscii(core::path::Extension(absolute)) != ".prca") {
            continue;
        }
        auto relative = RelativeTo(root, absolute);
        if (!relative) {
            return std::move(relative).TakeError();
        }
        const std::string stem = StemOf(relative.Value());
        auto guid = core::Guid128::FromHex(stem);
        if (!guid) {
            return MakeError(ErrorCode::Corrupt,
                             "cooked asset filename is not a GUID: " + relative.Value())
                .WithPath(relative.Value());
        }
        result.push_back(CookedFile{relative.Value(), absolute, guid.Value()});
    }
    std::sort(result.begin(), result.end(), [](const CookedFile& left, const CookedFile& right) {
        return left.relative_path < right.relative_path;
    });
    return result;
}

Result<AssetData> LoadAssetData(const CookedFile& file, const CollectorConfig& collector) {
    auto bytes = ReadFileBytes(file.absolute_path);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    auto reader = serialize::AssetFileReader::Open(bytes.Value(), true);
    if (!reader) {
        return std::move(reader).TakeError();
    }
    PARCEL_TRY(reader.Value().ReadAll(true));
    if (reader.Value().Objects().size() != 1) {
        return MakeError(ErrorCode::Unsupported,
                         "pack requires exactly one object per cooked asset")
            .WithPath(file.relative_path);
    }
    const auto objects = reader.Value().Objects();
    const auto types = reader.Value().Types();
    const auto type_entry =
        std::find_if(types.begin(), types.end(), [&](const serialize::AssetTypeView& entry) {
            return entry.type_id == objects.front().type_id;
        });
    if (type_entry == types.end() || type_entry->type_name.empty()) {
        return MakeError(ErrorCode::Corrupt, "cooked asset type is not declared")
            .WithPath(file.relative_path);
    }

    AssetData asset;
    asset.guid = file.guid;
    asset.relative_path = file.relative_path;
    asset.absolute_path = file.absolute_path;
    asset.type_name.assign(type_entry->type_name);
    asset.collector_type = collector.type;
    asset.address_rule = collector.address_rule;
    asset.address_prefix = collector.address_prefix;
    asset.explicit_address = collector.address;
    asset.tags = collector.tags;
    asset.bundle_hint = collector.bundle;
    asset.share = collector.share;
    asset.local_object_id = objects.front().local_id;
    for (const serialize::AssetExternalRefView& ref : reader.Value().ExternalRefs()) {
        if (ref.asset_guid.IsZero()) {
            return MakeError(ErrorCode::Corrupt, "cooked asset has a zero external reference")
                .WithPath(file.relative_path);
        }
        if (std::find(asset.dependencies.begin(), asset.dependencies.end(), ref.asset_guid) ==
            asset.dependencies.end()) {
            asset.dependencies.push_back(ref.asset_guid);
        }
    }
    std::sort(asset.dependencies.begin(), asset.dependencies.end());
    asset.bytes = std::move(bytes).Value();
    asset.hash = core::Sha256(asset.bytes);
    return asset;
}

bool IsUnderPath(std::string_view root, std::string_view path) {
    return core::path::IsWithin(root, path, true);
}

Result<std::vector<std::string>> FilesForCollector(const PackContext& context,
                                                   const CollectorConfig& collector) {
    std::string collector_root = context.cooked_root;
    if (!collector.path.empty()) {
        auto resolved = core::path::ResolveWithin(context.cooked_root, collector.path);
        if (!resolved) {
            return std::move(resolved).TakeError();
        }
        collector_root = resolved.Value();
    }

    std::vector<std::string> result;
    std::error_code error;
    const std::filesystem::path root_path = Utf8Path(collector_root);
    if (!collector.files.empty()) {
        for (const std::string& file : collector.files) {
            auto resolved = core::path::ResolveWithin(collector_root, file);
            if (!resolved) {
                return std::move(resolved).TakeError();
            }
            const auto stat = std::filesystem::status(Utf8Path(resolved.Value()), error);
            if (error || !std::filesystem::is_regular_file(stat)) {
                return MakeError(ErrorCode::NotFound,
                                 "collector file does not exist: " + resolved.Value())
                    .WithPath(resolved.Value());
            }
            result.push_back(NormalizePath(resolved.Value()));
        }
    } else {
        const auto stat = std::filesystem::status(root_path, error);
        if (error || !std::filesystem::exists(stat)) {
            return MakeError(ErrorCode::NotFound,
                             "collector path does not exist: " + collector_root)
                .WithPath(collector_root);
        }
        if (std::filesystem::is_regular_file(stat)) {
            result.push_back(NormalizePath(collector_root));
        } else if (std::filesystem::is_directory(stat)) {
            if (collector.recursive) {
                std::filesystem::recursive_directory_iterator iterator(root_path, error);
                if (error) {
                    return MakeError(ErrorCode::Io,
                                     "failed to scan collector directory: " + error.message())
                        .WithPath(collector_root);
                }
                const std::filesystem::recursive_directory_iterator end;
                for (; iterator != end; iterator.increment(error)) {
                    if (error) {
                        return MakeError(ErrorCode::Io,
                                         "failed to scan collector directory: " + error.message())
                            .WithPath(collector_root);
                    }
                    if (iterator->is_regular_file(error)) {
                        result.push_back(NormalizePath(FromPath(iterator->path())));
                    }
                    if (error) {
                        error.clear();
                    }
                }
            } else {
                std::filesystem::directory_iterator iterator(root_path, error);
                if (error) {
                    return MakeError(ErrorCode::Io,
                                     "failed to scan collector directory: " + error.message())
                        .WithPath(collector_root);
                }
                const std::filesystem::directory_iterator end;
                for (; iterator != end; iterator.increment(error)) {
                    if (error) {
                        return MakeError(ErrorCode::Io,
                                         "failed to scan collector directory: " + error.message())
                            .WithPath(collector_root);
                    }
                    if (iterator->is_regular_file(error)) {
                        result.push_back(NormalizePath(FromPath(iterator->path())));
                    }
                    if (error) {
                        error.clear();
                    }
                }
            }
        } else {
            return MakeError(ErrorCode::InvalidArgument,
                             "collector path is not a file or directory: " + collector_root)
                .WithPath(collector_root);
        }
    }

    std::vector<std::string> filtered;
    filtered.reserve(result.size());
    for (std::string& path : result) {
        if (LowerAscii(core::path::Extension(path)) == ".prca") {
            filtered.push_back(std::move(path));
        }
    }
    std::sort(filtered.begin(), filtered.end());
    filtered.erase(std::unique(filtered.begin(), filtered.end()), filtered.end());
    return filtered;
}

Result<void> AddSelection(PackContext& context, const CollectorConfig& collector,
                          const std::string& absolute_path) {
    auto relative = RelativeTo(context.cooked_root, absolute_path);
    if (!relative) {
        return std::move(relative).TakeError();
    }
    const std::string stem = StemOf(relative.Value());
    auto guid = core::Guid128::FromHex(stem);
    if (!guid) {
        return MakeError(ErrorCode::Corrupt,
                         "cooked asset filename is not a GUID: " + relative.Value())
            .WithPath(relative.Value());
    }
    if (!context.file_by_guid.contains(guid.Value())) {
        return MakeError(ErrorCode::Corrupt, "cooked asset was not discovered")
            .WithPath(relative.Value());
    }
    Selection selection;
    selection.collector = collector;
    selection.relative_path = relative.Value();
    context.selections[guid.Value()].push_back(std::move(selection));
    return {};
}

Result<void> CollectAssets(PackContext& context) {
    for (const CollectorConfig& collector : context.config.collectors) {
        if (!collector.name.empty() && HasNul(collector.name)) {
            return MakeError(ErrorCode::InvalidArgument, "collector name contains NUL");
        }
        if (collector.path.empty() && collector.files.empty()) {
            // Empty path means the cooked root.
        }
        auto files = FilesForCollector(context, collector);
        if (!files) {
            return std::move(files).TakeError();
        }
        for (const std::string& file : files.Value()) {
            auto added = AddSelection(context, collector, file);
            if (!added) {
                return std::move(added).TakeError();
            }
        }
    }
    if (context.selections.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "collector matched no cooked assets");
    }
    return {};
}

std::string MakeAddress(const AssetData& asset, const PackContext& context) {
    std::string value;
    switch (asset.address_rule) {
    case AddressRule::AssetPath:
        value = asset.relative_path;
        break;
    case AddressRule::FileName:
        value = StemOf(asset.relative_path);
        break;
    case AddressRule::Guid:
        value = asset.guid.ToHex();
        break;
    case AddressRule::Prefix:
        value = JoinName(asset.address_prefix, StemOf(asset.relative_path));
        break;
    case AddressRule::Address:
        value = asset.explicit_address;
        break;
    }
    if (context.config.location_to_lower) {
        value = LowerAscii(value);
    }
    return value;
}

Result<void> AutoCollectShaders(PackContext& context) {
    if (!context.config.auto_collect_shaders) {
        return {};
    }
    CollectorConfig fallback;
    fallback.name = "auto-shader";
    fallback.type = CollectorType::Depend;
    fallback.address_rule = AddressRule::AssetPath;
    fallback.share = true;
    for (const CookedFile& file : context.cooked_files) {
        if (context.assets.contains(file.guid)) {
            if (context.assets.at(file.guid).type_name == "ShaderLibraryAsset") {
                context.roots.insert(file.guid);
            }
            continue;
        }
        auto loaded = LoadAssetData(file, fallback);
        if (!loaded) {
            context.warnings.push_back("auto shader scan skipped " + file.relative_path + ": " +
                                       loaded.GetError().ToString());
            continue;
        }
        AssetData asset = std::move(loaded).Value();
        if (asset.type_name != "ShaderLibraryAsset") {
            continue;
        }
        asset.address = MakeAddress(asset, context);
        context.assets.emplace(file.guid, std::move(asset));
        context.roots.insert(file.guid);
    }
    return {};
}

Result<void> LoadSelectedAssets(PackContext& context) {
    for (const auto& [guid, selection_list] : context.selections) {
        if (selection_list.empty()) {
            continue;
        }
        const Selection* chosen = &selection_list.front();
        for (const Selection& candidate : selection_list) {
            if (CollectorPriority(candidate.collector.type) >
                CollectorPriority(chosen->collector.type)) {
                chosen = &candidate;
            }
        }
        if (selection_list.size() > 1) {
            context.warnings.push_back("asset matched multiple collectors: " +
                                       chosen->relative_path);
        }
        const auto file_it = context.file_by_guid.find(guid);
        if (file_it == context.file_by_guid.end()) {
            return MakeError(ErrorCode::Corrupt, "selected asset is missing from cooked files")
                .WithAsset(guid.ToHex());
        }
        CookedFile file;
        file.guid = guid;
        file.relative_path = chosen->relative_path;
        file.absolute_path = file_it->second;
        auto loaded = LoadAssetData(file, chosen->collector);
        if (!loaded) {
            return std::move(loaded).TakeError();
        }
        AssetData asset = std::move(loaded).Value();
        asset.collector_type = chosen->collector.type;
        asset.address_rule = chosen->collector.address_rule;
        asset.address_prefix = chosen->collector.address_prefix;
        asset.explicit_address = chosen->collector.address;
        asset.tags = chosen->collector.tags;
        asset.bundle_hint = chosen->collector.bundle;
        asset.share = chosen->collector.share;
        for (const Selection& selection : selection_list) {
            for (const std::string& tag : selection.collector.tags) {
                if (std::find(asset.tags.begin(), asset.tags.end(), tag) == asset.tags.end()) {
                    asset.tags.push_back(tag);
                }
            }
        }
        std::sort(asset.tags.begin(), asset.tags.end());
        asset.tags.erase(std::unique(asset.tags.begin(), asset.tags.end()), asset.tags.end());
        asset.address = MakeAddress(asset, context);
        if (asset.address_rule == AddressRule::Address && asset.address.empty()) {
            return MakeError(ErrorCode::InvalidArgument, "explicit asset address is empty")
                .WithAsset(guid.ToHex());
        }
        if (context.config.enable_addressable && (asset.address.empty() || HasNul(asset.address))) {
            return MakeError(ErrorCode::InvalidArgument, "asset address is empty or invalid")
                .WithAsset(guid.ToHex());
        }
        if (!asset.address.empty() && (HasNul(asset.address) || HasControl(asset.address))) {
            return MakeError(ErrorCode::InvalidArgument,
                             "asset address contains control characters")
                .WithAsset(guid.ToHex());
        }
        if (context.config.replace_asset_path_with_address &&
            !core::path::IsSafeRelative(asset.address)) {
            return MakeError(ErrorCode::InvalidArgument, "asset address is not a safe asset path")
                .WithAsset(guid.ToHex());
        }
        if (chosen->collector.type == CollectorType::Main ||
            chosen->collector.type == CollectorType::Static) {
            context.roots.insert(guid);
        }
        if (!core::path::IsSafeRelative(asset.relative_path) || HasControl(asset.relative_path)) {
            return MakeError(ErrorCode::InvalidArgument,
                             "cooked asset path is not a safe relative path")
                .WithAsset(guid.ToHex())
                .WithPath(asset.relative_path);
        }
        context.assets.emplace(guid, std::move(asset));
    }
    return {};
}

Result<void> EnsureDependency(PackContext& context, const core::AssetGuid& guid) {
    if (context.assets.contains(guid)) {
        return {};
    }
    const auto file_it = context.file_by_guid.find(guid);
    if (file_it == context.file_by_guid.end()) {
        return MakeError(ErrorCode::NotFound, "missing dependency asset").WithAsset(guid.ToHex());
    }
    CookedFile file;
    file.guid = guid;
    file.relative_path = "";
    auto relative = RelativeTo(context.cooked_root, file_it->second);
    if (!relative) {
        return std::move(relative).TakeError();
    }
    file.relative_path = relative.Value();
    file.absolute_path = file_it->second;
    CollectorConfig fallback;
    fallback.name = "dependency";
    fallback.type = CollectorType::Depend;
    fallback.address_rule = AddressRule::AssetPath;
    fallback.share = true;
    auto loaded = LoadAssetData(file, fallback);
    if (!loaded) {
        return std::move(loaded).TakeError();
    }
    AssetData asset = std::move(loaded).Value();
    asset.address = MakeAddress(asset, context);
    context.assets.emplace(guid, std::move(asset));
    return {};
}

Result<void> LoadDependencies(PackContext& context, const core::AssetGuid& guid,
                              std::set<core::AssetGuid>& visiting,
                              std::set<core::AssetGuid>& loaded) {
    if (loaded.contains(guid)) {
        return {};
    }
    if (!visiting.insert(guid).second) {
        return MakeError(ErrorCode::InvalidArgument, "cyclic asset dependency")
            .WithAsset(guid.ToHex());
    }
    const auto asset_it = context.assets.find(guid);
    if (asset_it == context.assets.end()) {
        return MakeError(ErrorCode::Internal, "asset was not loaded").WithAsset(guid.ToHex());
    }
    for (const core::AssetGuid& dependency : asset_it->second.dependencies) {
        if (dependency == guid) {
            return MakeError(ErrorCode::InvalidArgument, "asset depends on itself")
                .WithAsset(guid.ToHex());
        }
        auto ensured = EnsureDependency(context, dependency);
        if (!ensured) {
            return std::move(ensured).TakeError();
        }
        auto nested = LoadDependencies(context, dependency, visiting, loaded);
        if (!nested) {
            return std::move(nested).TakeError();
        }
    }
    visiting.erase(guid);
    loaded.insert(guid);
    return {};
}

Result<void> AnalyzeDependencies(PackContext& context) {
    if (context.roots.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "package has no main or static assets");
    }
    std::set<core::AssetGuid> visiting;
    std::set<core::AssetGuid> loaded;
    for (const core::AssetGuid& root : context.roots) {
        auto result = LoadDependencies(context, root, visiting, loaded);
        if (!result) {
            return std::move(result).TakeError();
        }
    }

    std::vector<core::AssetGuid> stack(context.roots.begin(), context.roots.end());
    while (!stack.empty()) {
        const core::AssetGuid guid = stack.back();
        stack.pop_back();
        if (!context.reachable.insert(guid).second) {
            continue;
        }
        const auto asset_it = context.assets.find(guid);
        if (asset_it == context.assets.end()) {
            return MakeError(ErrorCode::Internal, "reachable asset was not loaded")
                .WithAsset(guid.ToHex());
        }
        for (const core::AssetGuid& dependency : asset_it->second.dependencies) {
            stack.push_back(dependency);
        }
    }

    std::vector<core::AssetGuid> remove;
    for (const auto& [guid, asset] : context.assets) {
        if (!context.reachable.contains(guid)) {
            remove.push_back(guid);
            context.report.zero_reference_assets.push_back(asset.relative_path);
            context.warnings.push_back("removed zero-reference dependency: " + asset.relative_path);
        }
    }
    for (const core::AssetGuid& guid : remove) {
        context.assets.erase(guid);
    }
    if (context.assets.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "no assets remain after dependency analysis");
    }

    for (const auto& [guid, asset] : context.assets) {
        for (const core::AssetGuid& dependency : asset.dependencies) {
            if (!context.assets.contains(dependency)) {
                return MakeError(ErrorCode::NotFound, "missing dependency after analysis")
                    .WithAsset(guid.ToHex())
                    .WithPath(asset.relative_path);
            }
            context.referrers[dependency].push_back(guid);
        }
    }
    for (auto& [guid, referrers] : context.referrers) {
        (void)guid;
        std::sort(referrers.begin(), referrers.end());
        referrers.erase(std::unique(referrers.begin(), referrers.end()), referrers.end());
    }
    return {};
}

Result<std::string> AssignBundle(PackContext& context, const core::AssetGuid& guid,
                                 std::map<core::AssetGuid, int>& state) {
    const auto existing = context.bundle_of.find(guid);
    if (existing != context.bundle_of.end()) {
        return existing->second;
    }
    const auto state_it = state.find(guid);
    if (state_it != state.end() && state_it->second == 1) {
        return MakeError(ErrorCode::InvalidArgument, "cyclic bundle assignment")
            .WithAsset(guid.ToHex());
    }
    state[guid] = 1;
    const auto asset_it = context.assets.find(guid);
    if (asset_it == context.assets.end()) {
        return MakeError(ErrorCode::Internal, "asset missing during bundle assignment")
            .WithAsset(guid.ToHex());
    }
    const AssetData& asset = asset_it->second;
    std::string name;
    if (asset.type_name == "ShaderLibraryAsset") {
        name = asset.bundle_hint.empty() ? "shaders" : asset.bundle_hint;
    } else if (asset.type_name == "RawFileAsset") {
        name = asset.bundle_hint.empty() ? "raw/" + asset.guid.ToHex() : asset.bundle_hint;
    } else if (asset.collector_type == CollectorType::Depend) {
        if (!asset.bundle_hint.empty()) {
            name = asset.bundle_hint;
        } else {
            std::vector<core::AssetGuid> refs;
            const auto ref_it = context.referrers.find(guid);
            if (ref_it != context.referrers.end()) {
                refs = ref_it->second;
            }
            std::set<std::string> referrer_bundles;
            for (const core::AssetGuid& referrer : refs) {
                auto referrer_bundle = AssignBundle(context, referrer, state);
                if (!referrer_bundle) {
                    return std::move(referrer_bundle).TakeError();
                }
                referrer_bundles.insert(std::move(referrer_bundle).Value());
            }
            const bool share = context.config.enable_share_bundle && asset.share;
            if (share &&
                (referrer_bundles.size() > 1 ||
                 (referrer_bundles.size() == 1 && context.config.single_referenced_pack_alone))) {
                name = "shared/" + ParentBundleName(asset.relative_path);
            } else if (!referrer_bundles.empty()) {
                name = *referrer_bundles.begin();
                if (referrer_bundles.size() > 1) {
                    context.warnings.push_back(
                        "depend asset has multiple referrer bundles but sharing is disabled: " +
                        asset.relative_path);
                }
            } else {
                name = "shared/" + ParentBundleName(asset.relative_path);
            }
        }
    } else {
        name =
            asset.bundle_hint.empty() ? ParentBundleName(asset.relative_path) : asset.bundle_hint;
    }
    if (name.empty() || HasNul(name) || HasControl(name) || !core::path::IsSafeRelative(name)) {
        state.erase(guid);
        return MakeError(ErrorCode::InvalidArgument, "invalid bundle name")
            .WithAsset(guid.ToHex())
            .WithBundle(name);
    }
    context.bundle_of[guid] = name;
    state[guid] = 2;
    return name;
}

core::AssetGuid BundleGuidFor(const PackContext& context, const BundlePlan& bundle) {
    std::string seed = "parcel.bundle.v3|";
    seed += context.config.package_name;
    seed += "|";
    seed += bundle.name;
    seed += "|";
    seed += std::to_string(static_cast<std::uint32_t>(context.config.compression));
    seed += "|";
    seed += std::to_string(context.config.block_size);
    seed += "|";
    seed += std::to_string(context.config.compression_level);
    seed += "|";
    seed += std::string(kPackToolVersion);
    for (const core::AssetGuid& guid : bundle.members) {
        const auto asset_it = context.assets.find(guid);
        seed += "|";
        seed += guid.ToHex();
        seed += ":";
        seed += asset_it->second.hash.ToHex();
    }
    const core::Hash256 hash = core::Sha256(seed);
    std::array<std::byte, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(hash.bytes[i]);
    }
    return core::Guid128::FromBytes(std::span<const std::byte, 16>(bytes));
}

Result<void> BuildBundlePlans(PackContext& context) {
    std::map<core::AssetGuid, int> state;
    for (const auto& [guid, asset] : context.assets) {
        (void)asset;
        auto assigned = AssignBundle(context, guid, state);
        if (!assigned) {
            return std::move(assigned).TakeError();
        }
    }

    for (const auto& [guid, name] : context.bundle_of) {
        auto& bundle = context.bundles[name];
        if (bundle.name.empty()) {
            bundle.name = name;
        }
        bundle.members.push_back(guid);
    }
    for (auto& [name, bundle] : context.bundles) {
        (void)name;
        std::sort(bundle.members.begin(), bundle.members.end());
        std::set<std::string> tags;
        for (const core::AssetGuid& guid : bundle.members) {
            const AssetData& asset = context.assets.at(guid);
            tags.insert(asset.tags.begin(), asset.tags.end());
            if (asset.type_name == "RawFileAsset" && bundle.members.size() > 1) {
                return MakeError(ErrorCode::InvalidArgument,
                                 "raw bundle must contain exactly one asset")
                    .WithBundle(bundle.name)
                    .WithAsset(asset.relative_path);
            }
        }
        bundle.tags.assign(tags.begin(), tags.end());
        bundle.guid = BundleGuidFor(context, bundle);
        bundle.file_name = bundle.guid.ToHex() + ".bundle";
    }

    std::map<core::AssetGuid, std::string> guid_to_bundle;
    for (const auto& [name, bundle] : context.bundles) {
        (void)name;
        if (guid_to_bundle.contains(bundle.guid)) {
            return MakeError(ErrorCode::InvalidArgument, "bundle GUID collision")
                .WithBundle(bundle.name);
        }
        guid_to_bundle[bundle.guid] = bundle.name;
    }

    for (auto& [name, bundle] : context.bundles) {
        std::set<std::string> dependencies;
        for (const core::AssetGuid& member : bundle.members) {
            const AssetData& asset = context.assets.at(member);
            for (const core::AssetGuid& dependency : asset.dependencies) {
                const std::string& target = context.bundle_of.at(dependency);
                if (target != name) {
                    dependencies.insert(target);
                }
            }
        }
        bundle.dependency_names.assign(dependencies.begin(), dependencies.end());
    }

    for (auto& [name, bundle] : context.bundles) {
        std::set<std::string> referrers;
        for (const auto& [other_name, other] : context.bundles) {
            if (other_name == name) {
                continue;
            }
            if (std::find(other.dependency_names.begin(), other.dependency_names.end(), name) !=
                other.dependency_names.end()) {
                referrers.insert(other_name);
            }
        }
        for (const std::string& referrer : referrers) {
            bundle.referrer_guids.push_back(context.bundles.at(referrer).guid);
        }
        std::sort(bundle.referrer_guids.begin(), bundle.referrer_guids.end());
    }

    std::map<std::string, int> colors;
    for (const auto& [name, bundle] : context.bundles) {
        (void)bundle;
        colors[name] = 0;
    }
    auto visit = [&](auto&& self, const std::string& name) -> Result<void> {
        int& color = colors[name];
        if (color == 1) {
            return MakeError(ErrorCode::InvalidArgument, "cyclic bundle dependency")
                .WithBundle(name);
        }
        if (color == 2) {
            return {};
        }
        color = 1;
        for (const std::string& dependency : context.bundles.at(name).dependency_names) {
            auto result = self(self, dependency);
            if (!result) {
                return std::move(result).TakeError();
            }
        }
        color = 2;
        return {};
    };
    for (const auto& [name, bundle] : context.bundles) {
        (void)bundle;
        auto result = visit(visit, name);
        if (!result) {
            return std::move(result).TakeError();
        }
    }
    for (auto& [name, bundle] : context.bundles) {
        bundle.dependency_guids.clear();
        for (const std::string& dependency : bundle.dependency_names) {
            bundle.dependency_guids.push_back(context.bundles.at(dependency).guid);
        }
    }
    return {};
}

Result<BuildDB> LoadBuildDB(std::string_view path) {
    BuildDB db;
    core::PlatformFile platform;
    auto exists = platform.Exists(path);
    if (!exists) {
        return std::move(exists).TakeError();
    }
    if (!exists.Value()) {
        return db;
    }
    auto bytes = ReadFileBytes(path);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    const std::string text(reinterpret_cast<const char*>(bytes.Value().data()),
                           bytes.Value().size());
    auto parsed = assets::ParseJson(text);
    if (!parsed) {
        return MakeError(ErrorCode::Corrupt, "build database is not valid JSON")
            .WithPath(std::string(path));
    }
    const assets::JsonValue& root = parsed.Value();
    if (!root.IsObject()) {
        return MakeError(ErrorCode::Corrupt, "build database root is not an object")
            .WithPath(std::string(path));
    }
    PARCEL_TRY_DECL(std::uint32_t, version, ReadUInt(root, "version", kBuildDBVersion));
    if (version != kBuildDBVersion) {
        return MakeError(ErrorCode::Unsupported, "build database version is not supported")
            .WithPath(std::string(path));
    }
    db.version = version;
    PARCEL_TRY_ASSIGN(db.tool_version,
                      ReadString(root, "toolVersion", std::string(kPackToolVersion)));
    PARCEL_TRY_ASSIGN(db.package_name, ReadString(root, "package"));
    PARCEL_TRY_ASSIGN(db.package_version, ReadString(root, "packageVersion"));
    PARCEL_TRY_ASSIGN(db.target, ReadString(root, "target"));
    const assets::JsonValue* bundles = FindField(root, "bundles");
    if (bundles == nullptr) {
        return db;
    }
    if (!bundles->IsArray()) {
        return MakeError(ErrorCode::Corrupt, "build database bundles must be an array")
            .WithPath(std::string(path));
    }
    for (const assets::JsonValue& item : bundles->array_value) {
        if (!item.IsObject()) {
            return MakeError(ErrorCode::Corrupt, "build database bundle is not an object")
                .WithPath(std::string(path));
        }
        BuildDBBundle bundle;
        PARCEL_TRY_ASSIGN(bundle.name, ReadString(item, "name", {}, true));
        PARCEL_TRY_DECL(std::string, guid_text, ReadString(item, "guid", {}, true));
        PARCEL_TRY_DECL(std::string, file_name, ReadString(item, "fileName", {}, true));
        PARCEL_TRY_DECL(std::string, hash_text, ReadString(item, "fileHash", {}, true));
        auto guid = core::Guid128::FromHex(guid_text);
        if (!guid) {
            return std::move(guid).TakeError();
        }
        auto hash = core::Hash256::FromHex(hash_text);
        if (!hash) {
            return std::move(hash).TakeError();
        }
        bundle.guid = guid.Value();
        bundle.file_name = std::move(file_name);
        bundle.file_hash = hash.Value();
        const assets::JsonValue* members = FindField(item, "members");
        if (members == nullptr || !members->IsArray()) {
            return MakeError(ErrorCode::Corrupt, "build database members must be an array")
                .WithPath(std::string(path));
        }
        for (const assets::JsonValue& member : members->array_value) {
            if (!member.IsObject()) {
                return MakeError(ErrorCode::Corrupt, "build database member is not an object")
                    .WithPath(std::string(path));
            }
            PARCEL_TRY_DECL(std::string, member_guid_text, ReadString(member, "guid", {}, true));
            PARCEL_TRY_DECL(std::string, member_hash_text, ReadString(member, "hash", {}, true));
            auto member_guid = core::Guid128::FromHex(member_guid_text);
            if (!member_guid) {
                return std::move(member_guid).TakeError();
            }
            auto member_hash = core::Hash256::FromHex(member_hash_text);
            if (!member_hash) {
                return std::move(member_hash).TakeError();
            }
            bundle.members.emplace_back(member_guid.Value(), member_hash.Value());
        }
        std::sort(bundle.members.begin(), bundle.members.end(),
                  [](const auto& left, const auto& right) { return left.first < right.first; });
        db.bundles.push_back(std::move(bundle));
    }
    return db;
}

bool BuildDBMatches(const BuildDBBundle& db, const BundlePlan& plan) {
    if (db.name != plan.name || db.guid != plan.guid || db.file_name != plan.file_name ||
        db.members.size() != plan.members.size()) {
        return false;
    }
    for (std::size_t i = 0; i < plan.members.size(); ++i) {
        if (db.members[i].first != plan.members[i]) {
            return false;
        }
    }
    return true;
}

Result<bool> TryReuseBundle(PackContext& context, BundlePlan& bundle) {
    if (context.build_db.tool_version != kPackToolVersion) {
        return false;
    }
    const auto db_it =
        std::find_if(context.build_db.bundles.begin(), context.build_db.bundles.end(),
                     [&](const BuildDBBundle& entry) { return entry.name == bundle.name; });
    if (db_it == context.build_db.bundles.end() || !BuildDBMatches(*db_it, bundle)) {
        return false;
    }
    for (const auto& [member, hash] : db_it->members) {
        const auto asset_it = context.assets.find(member);
        if (asset_it == context.assets.end() || asset_it->second.hash != hash) {
            return false;
        }
    }
    auto path = core::path::Join(context.output_directory, "bundles/" + bundle.file_name);
    if (!path) {
        return std::move(path).TakeError();
    }
    core::PlatformFile platform;
    auto exists = platform.Exists(path.Value());
    if (!exists) {
        return std::move(exists).TakeError();
    }
    if (!exists.Value()) {
        return false;
    }
    auto bytes = ReadFileBytes(path.Value());
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    if (core::Sha256(bytes.Value()) != db_it->file_hash) {
        return false;
    }
    bundle.bytes = std::move(bytes).Value();
    bundle.file_size = bundle.bytes.size();
    bundle.file_hash = core::Sha256(bundle.bytes);
    bundle.file_crc = core::Crc32(bundle.bytes);
    bundle.reused = true;
    return true;
}

struct BuiltBundle {
    std::vector<std::byte> bytes;
    std::vector<serialize::BundlePayloadDesc> payloads;
};

Result<BuiltBundle> BuildOneBundle(const PackContext& context, const BundlePlan& bundle) {
    std::vector<std::byte> logical;
    serialize::BundleToc toc;
    toc.bundle_guid = bundle.guid;
    toc.bundle_name = bundle.name;
    toc.dependencies = bundle.dependency_guids;
    std::uint32_t payload_id = 0;
    for (const core::AssetGuid& member : bundle.members) {
        const auto asset_it = context.assets.find(member);
        if (asset_it == context.assets.end()) {
            return MakeError(ErrorCode::Internal, "bundle member is missing")
                .WithBundle(bundle.name)
                .WithAsset(member.ToHex());
        }
        const AssetData& asset = asset_it->second;
        const std::size_t padding = (8U - (logical.size() % 8U)) % 8U;
        logical.insert(logical.end(), padding, std::byte{0});
        serialize::BundlePayloadDesc payload;
        payload.payload_id = payload_id;
        payload.logical_offset = logical.size();
        payload.size = asset.bytes.size();
        payload.main_asset_guid = member;
        toc.payloads.push_back(payload);
        logical.insert(logical.end(), asset.bytes.begin(), asset.bytes.end());
        ++payload_id;
    }
    serialize::BundleWriteOptions options;
    options.block_size = context.config.block_size;
    options.file_codec = context.config.compression;
    options.toc_codec = context.config.compression == serialize::CompressionCodec::None
                            ? serialize::CompressionCodec::None
                            : context.config.compression;
    options.compression_level = context.config.compression_level;
    auto bytes = serialize::WriteBundle(toc, logical, options);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    BuiltBundle built;
    built.payloads = std::move(toc.payloads);
    built.bytes = std::move(bytes).Value();
    return built;
}

Result<void> BuildBundleBytes(PackContext& context) {
    std::vector<std::string> pending;
    for (auto& [name, bundle] : context.bundles) {
        (void)name;
        for (std::size_t i = 0; i < bundle.members.size(); ++i) {
            context.payload_of[bundle.members[i]] = {bundle.name, static_cast<std::uint32_t>(i)};
        }
        auto reused = TryReuseBundle(context, bundle);
        if (!reused) {
            return std::move(reused).TakeError();
        }
        if (reused.Value()) {
            context.report.reused_bundle_count++;
        } else {
            pending.push_back(bundle.name);
        }
    }
    if (pending.empty()) {
        return {};
    }

    std::vector<BuiltBundle> built(pending.size());
    std::vector<core::Error> errors(pending.size(), core::Error(ErrorCode::Ok));
    std::atomic_size_t next{0};
    const std::size_t worker_count =
        std::min<std::size_t>(std::max<std::uint32_t>(1, context.config.jobs), pending.size());
    auto worker = [&]() {
        for (;;) {
            const std::size_t index = next.fetch_add(1);
            if (index >= pending.size()) {
                return;
            }
            auto result = BuildOneBundle(context, context.bundles.at(pending[index]));
            if (!result) {
                errors[index] = std::move(result).TakeError();
            } else {
                built[index] = std::move(result).Value();
            }
        }
    };
    if (worker_count <= 1) {
        worker();
    } else {
        std::vector<std::thread> threads;
        threads.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            threads.emplace_back(worker);
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    }

    for (std::size_t i = 0; i < pending.size(); ++i) {
        if (errors[i].code != ErrorCode::Ok) {
            return std::move(errors[i]);
        }
        BundlePlan& bundle = context.bundles.at(pending[i]);
        bundle.payloads = std::move(built[i].payloads);
        bundle.bytes = std::move(built[i].bytes);
        bundle.file_size = bundle.bytes.size();
        bundle.file_hash = core::Sha256(bundle.bytes);
        bundle.file_crc = core::Crc32(bundle.bytes);
    }
    return {};
}

Result<std::vector<std::byte>> BuildManifestBytes(PackContext& context) {
    serialize::ManifestDesc desc;
    desc.package_name = context.config.package_name;
    desc.package_version = context.config.package_version;
    desc.flags = 0;
    if (context.config.enable_addressable) {
        desc.flags |= serialize::kManifestFlagEnableAddressable;
    }
    if (context.config.support_extensionless) {
        desc.flags |= serialize::kManifestFlagSupportExtensionless;
    }
    if (context.config.location_to_lower) {
        desc.flags |= serialize::kManifestFlagLocationToLower;
    }
    if (context.config.include_asset_guid) {
        desc.flags |= serialize::kManifestFlagIncludeAssetGuid;
    }
    if (context.config.replace_asset_path_with_address) {
        desc.flags |= serialize::kManifestFlagReplaceAssetPathWithAddress;
    }
    desc.output_name_style = context.config.output_name_style;
    desc.build_bundle_type = context.config.build_bundle_type;

    std::map<std::string, std::uint32_t> bundle_index;
    std::vector<std::string> bundle_names;
    for (const auto& [name, bundle] : context.bundles) {
        (void)bundle;
        bundle_names.push_back(name);
    }
    std::sort(bundle_names.begin(), bundle_names.end());
    for (std::size_t i = 0; i < bundle_names.size(); ++i) {
        bundle_index[bundle_names[i]] = static_cast<std::uint32_t>(i);
    }

    std::set<std::string> all_tags;
    for (const std::string& name : bundle_names) {
        const BundlePlan& bundle = context.bundles.at(name);
        serialize::ManifestBundleDesc out;
        out.bundle_guid = bundle.guid;
        out.bundle_name = bundle.name;
        out.file_name = bundle.file_name;
        out.file_size = bundle.file_size;
        out.file_sha256 = bundle.file_hash;
        out.file_crc = bundle.file_crc;
        out.flags = 0;
        for (const std::string& dependency : bundle.dependency_names) {
            out.dependent_bundle_ids.push_back(bundle_index.at(dependency));
        }
        for (const core::AssetGuid& referrer : bundle.referrer_guids) {
            const auto found =
                std::find_if(context.bundles.begin(), context.bundles.end(),
                             [&](const auto& entry) { return entry.second.guid == referrer; });
            if (found != context.bundles.end()) {
                out.referrer_bundle_ids.push_back(bundle_index.at(found->first));
            }
        }
        out.tags = bundle.tags;
        all_tags.insert(bundle.tags.begin(), bundle.tags.end());
        desc.bundles.push_back(std::move(out));
    }

    std::vector<core::AssetGuid> main_assets;
    for (const auto& [guid, asset] : context.assets) {
        if (asset.collector_type == CollectorType::Main) {
            main_assets.push_back(guid);
        }
    }
    std::sort(main_assets.begin(), main_assets.end());
    for (const core::AssetGuid& guid : main_assets) {
        const AssetData& asset = context.assets.at(guid);
        const std::string& bundle_name = context.bundle_of.at(guid);
        const auto payload_it = context.payload_of.find(guid);
        if (payload_it == context.payload_of.end()) {
            return MakeError(ErrorCode::Internal, "main asset has no bundle payload")
                .WithAsset(guid.ToHex());
        }
        serialize::ManifestAssetDesc out;
        out.asset_guid = guid;
        out.address = asset.address;
        out.asset_path =
            context.config.replace_asset_path_with_address ? asset.address : asset.relative_path;
        out.type_name = asset.type_name;
        out.bundle_id = bundle_index.at(bundle_name);
        out.payload_id = payload_it->second.second;
        out.local_object_id = asset.local_object_id;
        out.tags = asset.tags;
        for (const core::AssetGuid& dependency : asset.dependencies) {
            const std::string& target = context.bundle_of.at(dependency);
            if (target != bundle_name) {
                const std::uint32_t id = bundle_index.at(target);
                if (std::find(out.dependent_bundle_ids.begin(), out.dependent_bundle_ids.end(),
                              id) == out.dependent_bundle_ids.end()) {
                    out.dependent_bundle_ids.push_back(id);
                }
            }
        }
        std::sort(out.dependent_bundle_ids.begin(), out.dependent_bundle_ids.end());
        all_tags.insert(asset.tags.begin(), asset.tags.end());
        desc.assets.push_back(std::move(out));
    }
    desc.tags.assign(all_tags.begin(), all_tags.end());
    return serialize::WriteManifest(desc);
}

Result<std::vector<std::byte>> BuildBuiltinCatalogBytes(const PackContext& context) {
    serialize::BuiltinCatalog catalog;
    catalog.package_name = context.config.package_name;
    catalog.package_version = context.config.package_version;

    std::vector<const BundlePlan*> bundles;
    bundles.reserve(context.bundles.size());
    for (const auto& [name, bundle] : context.bundles) {
        (void)name;
        bundles.push_back(&bundle);
    }
    std::sort(bundles.begin(), bundles.end(), [](const BundlePlan* left, const BundlePlan* right) {
        if (left->file_name != right->file_name) {
            return left->file_name < right->file_name;
        }
        return left->guid.ToHex() < right->guid.ToHex();
    });

    std::set<std::string> file_names;
    catalog.entries.reserve(bundles.size());
    for (const BundlePlan* bundle : bundles) {
        if (!file_names.insert(bundle->file_name).second) {
            return MakeError(ErrorCode::InvalidArgument,
                             "duplicate bundle file name in builtin catalog")
                .WithBundle(bundle->name)
                .WithPath(bundle->file_name);
        }
        catalog.entries.push_back({bundle->guid, bundle->file_name});
    }
    return serialize::EncodeBuiltinCatalog(catalog);
}

void CollectReportStats(PackContext& context) {
    std::map<std::string, std::uint32_t> type_counts;
    std::map<std::string, std::uint32_t> texture_formats;
    assets::TypeRegistry registry;
    if (auto registered = assets::RegisterStandardAssetSerializers(registry); !registered) {
        context.warnings.push_back("failed to register asset serializers for report statistics: " +
                                   registered.GetError().ToString());
    }
    std::map<std::string, std::vector<core::AssetGuid>> by_hash;
    for (const auto& [guid, asset] : context.assets) {
        type_counts[asset.type_name]++;
        by_hash[asset.hash.ToHex()].push_back(guid);
        context.report.total_asset_bytes += asset.bytes.size();
        if (asset.type_name != "TextureAsset" && asset.type_name != "ShaderLibraryAsset") {
            continue;
        }
        auto decoded = assets::DecodeAssetBytes(asset.bytes, registry, nullptr, true);
        if (!decoded) {
            context.warnings.push_back("failed to decode " + asset.type_name + " for report: " +
                                       asset.relative_path + ": " + decoded.GetError().ToString());
            continue;
        }
        if (asset.type_name == "TextureAsset") {
            const auto* texture = static_cast<const assets::TextureAsset*>(decoded.Value().get());
            std::string format;
            switch (texture->format) {
            case assets::TextureFormat::RGBA8:
                format = "rgba8";
                break;
            case assets::TextureFormat::BGRA8:
                format = "bgra8";
                break;
            case assets::TextureFormat::RGBA16F:
                format = "rgba16f";
                break;
            case assets::TextureFormat::RGBA32F:
                format = "rgba32f";
                break;
            case assets::TextureFormat::BC1:
                format = "bc1";
                break;
            case assets::TextureFormat::BC3:
                format = "bc3";
                break;
            case assets::TextureFormat::BC5:
                format = "bc5";
                break;
            case assets::TextureFormat::BC7:
                format = "bc7";
                break;
            }
            texture_formats[format]++;
        } else {
            const auto* shader =
                static_cast<const assets::ShaderLibraryAsset*>(decoded.Value().get());
            context.report.shader_backends += static_cast<std::uint32_t>(shader->backends.size());
            for (const assets::ShaderBackendLibrary& backend : shader->backends) {
                context.report.shader_variants +=
                    static_cast<std::uint32_t>(backend.variants.size());
            }
        }
    }
    context.report.asset_types.assign(type_counts.begin(), type_counts.end());
    context.report.texture_formats.assign(texture_formats.begin(), texture_formats.end());

    for (const auto& [hash, guids] : by_hash) {
        (void)hash;
        if (guids.size() <= 1) {
            continue;
        }
        std::string message = "duplicate asset content:";
        for (const core::AssetGuid& guid : guids) {
            message += " ";
            message += guid.ToHex();
        }
        context.report.duplicate_assets.push_back(message);
        context.warnings.push_back(message);
    }

    std::set<std::string> retained_paths;
    for (const auto& [name, bundle] : context.bundles) {
        (void)name;
        for (const core::AssetGuid& guid : bundle.members) {
            retained_paths.insert(context.assets.at(guid).relative_path);
        }
    }
    for (const CookedFile& file : context.cooked_files) {
        if (!retained_paths.contains(file.relative_path)) {
            context.report.unused_assets.push_back(file.relative_path);
        }
    }
    std::sort(context.report.unused_assets.begin(), context.report.unused_assets.end());
}

void FillReport(PackContext& context) {
    context.report.tool_version = std::string(kPackToolVersion);
    context.report.package_name = context.config.package_name;
    context.report.package_version = context.config.package_version;
    context.report.target = context.config.target;
    context.report.compression = LowerAscii(serialize::ToString(context.config.compression));
    context.report.block_size = context.config.block_size;
    context.report.compression_level = context.config.compression_level;
    context.report.enable_addressable = context.config.enable_addressable;
    context.report.enable_share_bundle = context.config.enable_share_bundle;
    context.report.single_referenced_pack_alone = context.config.single_referenced_pack_alone;
    context.report.auto_collect_shaders = context.config.auto_collect_shaders;
    context.report.asset_count = static_cast<std::uint32_t>(context.assets.size());
    context.report.bundle_count = static_cast<std::uint32_t>(context.bundles.size());
    for (const auto& [guid, asset] : context.assets) {
        if (asset.collector_type == CollectorType::Main) {
            context.report.main_asset_count++;
        }
        PackAssetReport out;
        out.guid = guid;
        out.path = asset.relative_path;
        out.type = asset.type_name;
        out.address = asset.address;
        out.bundle = context.bundle_of.at(guid);
        out.size = asset.bytes.size();
        out.dependency_count = static_cast<std::uint32_t>(asset.dependencies.size());
        out.main = asset.collector_type == CollectorType::Main;
        context.report.assets.push_back(std::move(out));
    }
    for (const auto& [name, bundle] : context.bundles) {
        (void)name;
        context.report.total_bundle_bytes += bundle.file_size;
        PackBundleReport out;
        out.guid = bundle.guid;
        out.name = bundle.name;
        out.file_name = bundle.file_name;
        out.size = bundle.file_size;
        out.asset_count = static_cast<std::uint32_t>(bundle.members.size());
        out.reused = bundle.reused;
        out.dependencies = bundle.dependency_names;
        context.report.bundles.push_back(std::move(out));
    }
    std::sort(context.report.assets.begin(), context.report.assets.end(),
              [](const PackAssetReport& left, const PackAssetReport& right) {
                  return left.path < right.path;
              });
    std::sort(context.report.bundles.begin(), context.report.bundles.end(),
              [](const PackBundleReport& left, const PackBundleReport& right) {
                  return left.name < right.name;
              });
    context.report.warnings = context.warnings;
}

Result<void> WriteBundleFiles(PackContext& context) {
    auto bundle_directory = core::path::Join(context.output_directory, "bundles");
    if (!bundle_directory) {
        return std::move(bundle_directory).TakeError();
    }
    PARCEL_TRY(EnsureDirectory(bundle_directory.Value()));
    for (const auto& [name, bundle] : context.bundles) {
        (void)name;
        if (bundle.reused) {
            continue;
        }
        auto path = core::path::Join(bundle_directory.Value(), bundle.file_name);
        if (!path) {
            return std::move(path).TakeError();
        }
        PARCEL_TRY(WriteBytesAtomic(path.Value(), bundle.bytes));
    }
    return {};
}

Result<void> CleanOldBundles(PackContext& context) {
    if (!context.config.clean_output) {
        return {};
    }
    auto bundle_directory = core::path::Join(context.output_directory, "bundles");
    if (!bundle_directory) {
        return std::move(bundle_directory).TakeError();
    }
    std::error_code error;
    std::filesystem::directory_iterator iterator(Utf8Path(bundle_directory.Value()), error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to scan bundle directory: " + error.message())
            .WithPath(bundle_directory.Value());
    }
    std::set<std::string> keep;
    for (const auto& [name, bundle] : context.bundles) {
        (void)name;
        keep.insert(bundle.file_name);
    }
    const std::filesystem::directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            return MakeError(ErrorCode::Io, "failed to scan bundle directory: " + error.message())
                .WithPath(bundle_directory.Value());
        }
        if (!iterator->is_regular_file(error)) {
            if (error) {
                error.clear();
            }
            continue;
        }
        const std::string name = FromPath(iterator->path().filename());
        if (LowerAscii(core::path::Extension(name)) == ".bundle" && !keep.contains(name)) {
            PARCEL_TRY(core::PlatformFile{}.Delete(NormalizePath(FromPath(iterator->path()))));
        }
    }
    return {};
}

assets::JsonValue BuildReportJson(const PackReport& report) {
    assets::JsonValue root;
    root.type = assets::JsonType::Object;
    AddNumber(root, "version", 1);
    AddString(root, "toolVersion", report.tool_version);
    AddString(root, "packageName", report.package_name);
    AddString(root, "packageVersion", report.package_version);
    AddString(root, "target", report.target);
    AddString(root, "compression", report.compression);
    AddNumber(root, "blockSize", report.block_size);
    AddNumber(root, "compressionLevel", report.compression_level);
    AddBool(root, "enableAddressable", report.enable_addressable);
    AddBool(root, "enableShareBundle", report.enable_share_bundle);
    AddBool(root, "singleReferencedPackAlone", report.single_referenced_pack_alone);
    AddBool(root, "autoCollectShaders", report.auto_collect_shaders);
    AddNumber(root, "assetCount", report.asset_count);
    AddNumber(root, "mainAssetCount", report.main_asset_count);
    AddNumber(root, "bundleCount", report.bundle_count);
    AddNumber(root, "reusedBundleCount", report.reused_bundle_count);
    AddNumber(root, "totalAssetBytes", static_cast<double>(report.total_asset_bytes));
    AddNumber(root, "totalBundleBytes", static_cast<double>(report.total_bundle_bytes));
    AddNumber(root, "manifestBytes", static_cast<double>(report.manifest_bytes));
    AddNumber(root, "builtinCatalogBytes", static_cast<double>(report.builtin_catalog_bytes));
    AddNumber(root, "shaderVariants", report.shader_variants);
    AddNumber(root, "shaderBackends", report.shader_backends);

    auto pairs_json = [](const std::vector<std::pair<std::string, std::uint32_t>>& values) {
        assets::JsonValue array;
        array.type = assets::JsonType::Array;
        for (const auto& [name, count] : values) {
            assets::JsonValue item;
            item.type = assets::JsonType::Object;
            AddString(item, "name", name);
            AddNumber(item, "count", count);
            array.array_value.push_back(std::move(item));
        }
        return array;
    };
    root.object_value.emplace_back("assetTypes", pairs_json(report.asset_types));
    root.object_value.emplace_back("textureFormats", pairs_json(report.texture_formats));

    assets::JsonValue assets_value;
    assets_value.type = assets::JsonType::Array;
    for (const PackAssetReport& asset : report.assets) {
        assets::JsonValue item;
        item.type = assets::JsonType::Object;
        AddString(item, "guid", asset.guid.ToHex());
        AddString(item, "path", asset.path);
        AddString(item, "type", asset.type);
        AddString(item, "address", asset.address);
        AddString(item, "bundle", asset.bundle);
        AddNumber(item, "size", static_cast<double>(asset.size));
        AddNumber(item, "dependencyCount", asset.dependency_count);
        AddBool(item, "main", asset.main);
        assets_value.array_value.push_back(std::move(item));
    }

    assets::JsonValue bundles_value;
    bundles_value.type = assets::JsonType::Array;
    for (const PackBundleReport& bundle : report.bundles) {
        assets::JsonValue item;
        item.type = assets::JsonType::Object;
        AddString(item, "guid", bundle.guid.ToHex());
        AddString(item, "name", bundle.name);
        AddString(item, "fileName", bundle.file_name);
        AddNumber(item, "size", static_cast<double>(bundle.size));
        AddNumber(item, "assetCount", bundle.asset_count);
        AddBool(item, "reused", bundle.reused);
        item.object_value.emplace_back("dependencies", StringArray(bundle.dependencies));
        bundles_value.array_value.push_back(std::move(item));
    }

    root.object_value.emplace_back("assets", std::move(assets_value));
    root.object_value.emplace_back("bundles", std::move(bundles_value));
    root.object_value.emplace_back("unusedAssets", StringArray(report.unused_assets));
    root.object_value.emplace_back("zeroReferenceAssets",
                                   StringArray(report.zero_reference_assets));
    root.object_value.emplace_back("duplicateAssets", StringArray(report.duplicate_assets));
    root.object_value.emplace_back("warnings", StringArray(report.warnings));
    root.object_value.emplace_back("errors", StringArray(report.errors));
    return root;
}

Result<void> WriteBuildReport(const std::string& output_directory, const PackReport& report) {
    PARCEL_TRY(EnsureDirectory(output_directory));
    auto path = core::path::Join(output_directory, "build_report.json");
    if (!path) {
        return std::move(path).TakeError();
    }
    const std::string text = assets::SerializeJson(BuildReportJson(report), true);
    return WriteTextFile(path.Value(), text);
}

assets::JsonValue BuildDBJson(const PackContext& context) {
    assets::JsonValue root;
    root.type = assets::JsonType::Object;
    AddNumber(root, "version", kBuildDBVersion);
    AddString(root, "toolVersion", std::string(kPackToolVersion));
    AddString(root, "package", context.config.package_name);
    AddString(root, "packageVersion", context.config.package_version);
    AddString(root, "target", context.config.target);
    assets::JsonValue bundles;
    bundles.type = assets::JsonType::Array;
    for (const auto& [name, bundle] : context.bundles) {
        (void)name;
        assets::JsonValue item;
        item.type = assets::JsonType::Object;
        AddString(item, "name", bundle.name);
        AddString(item, "guid", bundle.guid.ToHex());
        AddString(item, "fileName", bundle.file_name);
        AddString(item, "fileHash", bundle.file_hash.ToHex());
        assets::JsonValue members;
        members.type = assets::JsonType::Array;
        for (const core::AssetGuid& guid : bundle.members) {
            assets::JsonValue member;
            member.type = assets::JsonType::Object;
            AddString(member, "guid", guid.ToHex());
            AddString(member, "hash", context.assets.at(guid).hash.ToHex());
            members.array_value.push_back(std::move(member));
        }
        item.object_value.emplace_back("members", std::move(members));
        bundles.array_value.push_back(std::move(item));
    }
    root.object_value.emplace_back("bundles", std::move(bundles));
    return root;
}

Result<void> WriteBuildDB(const PackContext& context) {
    auto path = core::path::Join(context.output_directory, "build_db.json");
    if (!path) {
        return std::move(path).TakeError();
    }
    const std::string text = assets::SerializeJson(BuildDBJson(context), true);
    return WriteTextFile(path.Value(), text);
}

Result<void> Build(PackContext& context) {
    auto cooked = ResolveConfiguredPath(context, context.config.cooked_root);
    if (!cooked) {
        return std::move(cooked).TakeError();
    }
    context.cooked_root = std::move(cooked).Value();
    if (context.cooked_root.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "cooked root is empty");
    }
    auto output = ResolveConfiguredPath(context, context.config.output_root);
    if (!output) {
        return std::move(output).TakeError();
    }
    context.output_root = std::move(output).Value();
    if (context.output_root.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "output root is empty");
    }
    auto output_directory = MakeOutputDirectory(context);
    if (!output_directory) {
        return std::move(output_directory).TakeError();
    }
    context.output_directory = std::move(output_directory).Value();

    core::PlatformFile platform;
    auto cooked_exists = platform.Stat(context.cooked_root);
    if (!cooked_exists) {
        return std::move(cooked_exists).TakeError();
    }
    if (!cooked_exists.Value().is_directory) {
        return MakeError(ErrorCode::InvalidArgument, "cooked root is not a directory")
            .WithPath(context.cooked_root);
    }

    auto files = ScanCookedFiles(context.cooked_root);
    if (!files) {
        return std::move(files).TakeError();
    }
    context.cooked_files = std::move(files).Value();
    for (const CookedFile& file : context.cooked_files) {
        const auto [it, inserted] = context.file_by_guid.emplace(file.guid, file.absolute_path);
        if (!inserted) {
            return MakeError(ErrorCode::InvalidArgument, "duplicate asset GUID in cooked directory")
                .WithAsset(file.guid.ToHex())
                .WithPath(file.relative_path);
        }
    }

    PARCEL_TRY(CollectAssets(context));
    PARCEL_TRY(LoadSelectedAssets(context));
    PARCEL_TRY(AutoCollectShaders(context));
    PARCEL_TRY(AnalyzeDependencies(context));
    PARCEL_TRY(BuildBundlePlans(context));

    context.build_db = BuildDB{};
    context.build_db.package_name = context.config.package_name;
    context.build_db.package_version = context.config.package_version;
    context.build_db.target = context.config.target;
    auto db_path = core::path::Join(context.output_directory, "build_db.json");
    if (!db_path) {
        return std::move(db_path).TakeError();
    }
    auto previous_db = LoadBuildDB(db_path.Value());
    if (previous_db) {
        if (previous_db.Value().package_name == context.config.package_name &&
            previous_db.Value().package_version == context.config.package_version &&
            previous_db.Value().target == context.config.target) {
            context.build_db = std::move(previous_db).Value();
        }
    } else {
        context.warnings.push_back("ignored invalid build database: " +
                                   previous_db.GetError().ToString());
    }

    PARCEL_TRY(BuildBundleBytes(context));
    auto manifest = BuildManifestBytes(context);
    if (!manifest) {
        return std::move(manifest).TakeError();
    }
    context.report.manifest_bytes = manifest.Value().size();
    std::vector<std::byte> builtin_catalog;
    if (context.config.write_builtin_catalog) {
        auto catalog = BuildBuiltinCatalogBytes(context);
        if (!catalog) {
            return std::move(catalog).TakeError();
        }
        builtin_catalog = std::move(catalog).Value();
        context.report.builtin_catalog_bytes = builtin_catalog.size();
    }
    CollectReportStats(context);
    FillReport(context);

    PARCEL_TRY(EnsureDirectory(context.output_directory));
    PARCEL_TRY(WriteBundleFiles(context));
    auto manifest_path = core::path::Join(context.output_directory, "manifest.bytes");
    if (!manifest_path) {
        return std::move(manifest_path).TakeError();
    }
    PARCEL_TRY(WriteBytesAtomic(manifest_path.Value(), manifest.Value()));
    const core::Hash256 manifest_hash = core::Sha256(
        std::span<const std::byte>(manifest.Value()).subspan(serialize::kManifestHeaderSize));
    auto hash_path = core::path::Join(context.output_directory, "manifest.hash");
    if (!hash_path) {
        return std::move(hash_path).TakeError();
    }
    PARCEL_TRY(WriteTextFile(hash_path.Value(), manifest_hash.ToHex() + "\n"));
    auto version_path =
        core::path::Join(context.output_directory, context.config.package_name + ".version");
    if (!version_path) {
        return std::move(version_path).TakeError();
    }
    PARCEL_TRY(WriteTextFile(version_path.Value(), context.config.package_version + "\n"));
    if (context.config.write_builtin_catalog) {
        auto catalog_path =
            core::path::Join(context.output_directory, serialize::kBuiltinCatalogFileName);
        if (!catalog_path) {
            return std::move(catalog_path).TakeError();
        }
        PARCEL_TRY(WriteBytesAtomic(catalog_path.Value(), builtin_catalog));
    }
    PARCEL_TRY(CleanOldBundles(context));
    PARCEL_TRY(WriteBuildDB(context));
    return {};
}

} // namespace

Result<PackSummary> RunPack(const PackConfig& input) {
    PackConfig config = input;
    if (config.jobs == 0) {
        return MakeError(ErrorCode::InvalidArgument, "jobs must be greater than zero");
    }
    if (config.block_size == 0 || config.block_size > serialize::kMaxBlockSize) {
        return MakeError(ErrorCode::InvalidArgument, "invalid bundle block size");
    }
    if (config.compression_level < 0 || config.compression_level > 22) {
        return MakeError(ErrorCode::InvalidArgument, "invalid compression level");
    }
    if (config.enable_addressable && config.location_to_lower) {
        return MakeError(ErrorCode::InvalidArgument,
                         "addressable mode does not support lower-case locations");
    }
    if (config.replace_asset_path_with_address && !config.enable_addressable) {
        return MakeError(ErrorCode::InvalidArgument,
                         "replace asset path requires addressable mode");
    }
    if (config.collectors.empty()) {
        CollectorConfig fallback;
        fallback.name = "main";
        fallback.type = CollectorType::Main;
        config.collectors.push_back(std::move(fallback));
    }

    PackContext context;
    context.config = std::move(config);
    context.project_root = context.config.project_root;
    if (context.project_root.empty()) {
        auto current = AbsolutePath(".");
        if (!current) {
            return std::move(current).TakeError();
        }
        context.project_root = std::move(current).Value();
    } else {
        auto absolute = AbsolutePath(context.project_root);
        if (!absolute) {
            return std::move(absolute).TakeError();
        }
        context.project_root = std::move(absolute).Value();
    }

    auto output = ResolveConfiguredPath(context, context.config.output_root);
    if (!output) {
        return std::move(output).TakeError();
    }
    context.output_root = std::move(output).Value();
    if (context.output_root.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "output root is empty");
    }
    auto output_directory = MakeOutputDirectory(context);
    if (!output_directory) {
        return std::move(output_directory).TakeError();
    }
    context.output_directory = std::move(output_directory).Value();

    auto result = Build(context);
    if (!result) {
        context.report.tool_version = std::string(kPackToolVersion);
        context.report.package_name = context.config.package_name;
        context.report.package_version = context.config.package_version;
        context.report.target = context.config.target;
        context.report.compression = LowerAscii(serialize::ToString(context.config.compression));
        context.report.block_size = context.config.block_size;
        context.report.compression_level = context.config.compression_level;
        context.report.enable_addressable = context.config.enable_addressable;
        context.report.enable_share_bundle = context.config.enable_share_bundle;
        context.report.single_referenced_pack_alone = context.config.single_referenced_pack_alone;
        context.report.auto_collect_shaders = context.config.auto_collect_shaders;
        context.report.errors.push_back(result.GetError().ToString());
        auto report_written = WriteBuildReport(context.output_directory, context.report);
        if (!report_written) {
            context.warnings.push_back("failed to write failure report: " +
                                       report_written.GetError().ToString());
        }
        return std::move(result).TakeError();
    }

    auto report_written = WriteBuildReport(context.output_directory, context.report);
    if (!report_written) {
        return std::move(report_written).TakeError();
    }
    PackSummary summary;
    summary.output_directory = context.output_directory;
    summary.report = std::move(context.report);
    return summary;
}

} // namespace parcel::pack


