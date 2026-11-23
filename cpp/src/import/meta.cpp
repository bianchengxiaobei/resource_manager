#include "parcel/import/meta.hpp"

#include "parcel/core/error.hpp"
#include "parcel/core/path.hpp"
#include "parcel/core/platform_file.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <utility>

namespace parcel::import {
namespace {

using core::ErrorCode;
using core::MakeError;

core::Error Fail(std::string_view context, std::string message) {
    return MakeError(ErrorCode::InvalidArgument, std::string(context) + ": " + std::move(message));
}

core::Result<std::uint32_t> ReadU32(const assets::JsonValue& value, std::string_view context) {
    if (!value.IsNumber() || value.number_value < 0.0 ||
        value.number_value > std::numeric_limits<std::uint32_t>::max() ||
        std::floor(value.number_value) != value.number_value) {
        return Fail(context, "expected an unsigned integer");
    }
    return static_cast<std::uint32_t>(value.number_value);
}

core::Result<std::string> ReadString(const assets::JsonValue& value, std::string_view context) {
    if (!value.IsString() || value.string_value.empty()) {
        return Fail(context, "expected a non-empty string");
    }
    return value.string_value;
}

core::Result<core::AssetGuid> ReadGuid(const assets::JsonValue& value, std::string_view context) {
    if (!value.IsString()) {
        return Fail(context, "expected a GUID string");
    }
    auto guid = core::AssetGuid::FromHex(value.string_value);
    if (!guid) {
        return Fail(context, "invalid GUID");
    }
    if (guid.Value().IsZero()) {
        return Fail(context, "GUID must not be zero");
    }
    return guid;
}

core::Result<core::Hash256> ReadHash(const assets::JsonValue& value, std::string_view context) {
    if (!value.IsString()) {
        return Fail(context, "expected a hash string");
    }
    std::string_view text = value.string_value;
    constexpr std::string_view kPrefix = "sha256:";
    if (text.rfind(kPrefix, 0) == 0) {
        text.remove_prefix(kPrefix.size());
    }
    auto hash = core::Hash256::FromHex(text);
    if (!hash) {
        return Fail(context, "invalid SHA-256 hash");
    }
    return hash;
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

assets::JsonValue HashJson(const core::Hash256& hash) {
    assets::JsonValue field;
    field.type = assets::JsonType::String;
    field.string_value = "sha256:" + hash.ToHex();
    return field;
}

core::Result<void> EnsureParentDirectory(std::string_view path) {
    const std::string normalized = core::path::Normalize(path);
    const auto slash = normalized.find_last_of('/');
    if (slash == std::string::npos || slash == 0 || (slash == 2 && normalized[1] == ':')) {
        return {};
    }
    core::PlatformFile platform;
    return platform.CreateDirectories(normalized.substr(0, slash));
}

core::Result<void> WriteFile(std::string_view path, std::string_view text) {
    core::PlatformFile platform;
    PARCEL_TRY(EnsureParentDirectory(path));
    const std::string normalized = core::path::Normalize(path);
    const std::string temporary = normalized + ".tmp";
    PARCEL_TRY_DECL(std::unique_ptr<core::IFile>, file, platform.OpenWrite(temporary));
    PARCEL_TRY(core::WriteAll(*file, std::as_bytes(std::span(text.data(), text.size()))));
    PARCEL_TRY(file->Flush());
    file->Close();
    return platform.Rename(temporary, normalized, true);
}

} // namespace

const assets::JsonValue* FindSetting(const assets::JsonValue& settings,
                                     std::string_view key) noexcept {
    return settings.IsObject() ? settings.Find(key) : nullptr;
}

bool SettingBool(const assets::JsonValue& settings, std::string_view key, bool fallback) {
    const assets::JsonValue* value = FindSetting(settings, key);
    return value != nullptr && value->IsBool() ? value->bool_value : fallback;
}

double SettingNumber(const assets::JsonValue& settings, std::string_view key, double fallback) {
    const assets::JsonValue* value = FindSetting(settings, key);
    return value != nullptr && value->IsNumber() && std::isfinite(value->number_value)
               ? value->number_value
               : fallback;
}

std::string SettingString(const assets::JsonValue& settings, std::string_view key,
                          std::string_view fallback) {
    const assets::JsonValue* value = FindSetting(settings, key);
    return value != nullptr && value->IsString() ? value->string_value : std::string(fallback);
}

core::AssetGuid MakeSourceGuid(std::string_view project_relative_path) {
    const std::string normalized = core::path::Normalize(project_relative_path);
    const core::Hash256 hash = core::Sha256("parcel.source.v1:" + normalized);
    return core::AssetGuid::FromBytes(
        std::span<const std::byte, 16>(reinterpret_cast<const std::byte*>(hash.bytes.data()), 16));
}

core::AssetGuid MakeSubAssetGuid(core::AssetGuid source_guid, std::string_view role,
                                 std::string_view local_id) {
    const std::string material = "parcel.sub.v1:" + source_guid.ToHex() + "\x1f" +
                                 std::string(role) + "\x1f" + std::string(local_id);
    const core::Hash256 hash = core::Sha256(material);
    return core::AssetGuid::FromBytes(
        std::span<const std::byte, 16>(reinterpret_cast<const std::byte*>(hash.bytes.data()), 16));
}

assets::JsonValue MakeDefaultImportSettings(std::string_view importer, std::string_view target) {
    assets::JsonValue settings;
    settings.type = assets::JsonType::Object;
    auto add = [&](std::string key, assets::JsonValue value) {
        settings.object_value.emplace_back(std::move(key), std::move(value));
    };
    assets::JsonValue number;
    number.type = assets::JsonType::Number;
    number.number_value = 1.0;
    add("scale", number);
    number.number_value = 0.0;
    assets::JsonValue string;
    string.type = assets::JsonType::String;
    string.string_value = "rh-y-up-ccw";
    add("coordinateSystem", string);
    assets::JsonValue boolean;
    boolean.type = assets::JsonType::Bool;
    boolean.bool_value = true;
    add("generateTangents", boolean);
    if (importer == "texture") {
        add("sRGB", boolean);
        add("generateMips", boolean);
        string.string_value = "auto";
        add("format", string);
        string.string_value = "none";
        add("compression", string);
        string.string_value = "balanced";
        add("compressionQuality", string);
    }
    if (importer == "shader") {
        string.string_value = "hdrp";
        add("pipelineKey", string);
    }
    if (importer == "fbx" || importer == "gltf") {
        add("optimizeMesh", boolean);
        boolean.bool_value = false;
        add("optimizeOverdraw", boolean);
        number.number_value = 1.05;
        add("overdrawThreshold", number);
    }
    if (importer == "gltf") {
        string.string_value.clear();
        add("materialVariant", string);
    }
    string.string_value = std::string(target);
    add("target", string);
    return settings;
}

core::Hash256 HashSettings(const assets::JsonValue& settings) {
    const std::string text = assets::SerializeJson(settings, false);
    return core::Sha256(text);
}

std::string AssetMetaPath(std::string_view source_path) {
    return std::string(source_path) + ".parcelmeta";
}

core::Result<AssetMeta> ParseAssetMeta(std::string_view text) {
    auto parsed = assets::ParseJson(text);
    if (!parsed) {
        return std::move(parsed).TakeError();
    }
    const assets::JsonValue& root = parsed.Value();
    if (!root.IsObject()) {
        return Fail("meta", "expected an object");
    }

    const assets::JsonValue* version = root.Find("version");
    if (version == nullptr) {
        return Fail("version", "missing required field");
    }
    AssetMeta meta;
    PARCEL_TRY_ASSIGN(meta.version, ReadU32(*version, "version"));
    if (meta.version != kAssetMetaVersion) {
        return MakeError(ErrorCode::Unsupported, "unsupported .parcelmeta version");
    }

    const assets::JsonValue* source_guid = root.Find("sourceGuid");
    if (source_guid == nullptr) {
        return Fail("sourceGuid", "missing required field");
    }
    PARCEL_TRY_ASSIGN(meta.source_guid, ReadGuid(*source_guid, "sourceGuid"));

    const assets::JsonValue* importer = root.Find("importer");
    if (importer == nullptr) {
        return Fail("importer", "missing required field");
    }
    PARCEL_TRY_ASSIGN(meta.importer, ReadString(*importer, "importer"));

    const assets::JsonValue* importer_version = root.Find("importerVersion");
    if (importer_version == nullptr) {
        return Fail("importerVersion", "missing required field");
    }
    PARCEL_TRY_ASSIGN(meta.importer_version, ReadU32(*importer_version, "importerVersion"));

    if (const assets::JsonValue* source_hash = root.Find("sourceHash"); source_hash != nullptr) {
        PARCEL_TRY_ASSIGN(meta.source_hash, ReadHash(*source_hash, "sourceHash"));
    }
    if (const assets::JsonValue* settings_hash = root.Find("settingsHash");
        settings_hash != nullptr) {
        PARCEL_TRY_ASSIGN(meta.settings_hash, ReadHash(*settings_hash, "settingsHash"));
    }
    if (const assets::JsonValue* settings = root.Find("settings"); settings != nullptr) {
        if (!settings->IsObject()) {
            return Fail("settings", "expected an object");
        }
        meta.settings = *settings;
    } else {
        meta.settings.type = assets::JsonType::Object;
    }

    if (const assets::JsonValue* entries = root.Find("assets"); entries != nullptr) {
        if (!entries->IsArray()) {
            return Fail("assets", "expected an array");
        }
        for (std::size_t i = 0; i < entries->array_value.size(); ++i) {
            const assets::JsonValue& entry = entries->array_value[i];
            const std::string context = "assets[" + std::to_string(i) + "]";
            if (!entry.IsObject()) {
                return Fail(context, "expected an object");
            }
            MetaAsset asset;
            const assets::JsonValue* guid = entry.Find("guid");
            if (guid == nullptr) {
                return Fail(context + ".guid", "missing required field");
            }
            PARCEL_TRY_ASSIGN(asset.guid, ReadGuid(*guid, context + ".guid"));
            if (const assets::JsonValue* type = entry.Find("type"); type != nullptr) {
                PARCEL_TRY_ASSIGN(asset.type, ReadString(*type, context + ".type"));
            }
            if (const assets::JsonValue* role = entry.Find("role"); role != nullptr) {
                PARCEL_TRY_ASSIGN(asset.role, ReadString(*role, context + ".role"));
            }
            if (const assets::JsonValue* local_id = entry.Find("localId"); local_id != nullptr) {
                PARCEL_TRY_ASSIGN(asset.local_id, ReadString(*local_id, context + ".localId"));
            }
            const auto duplicate = std::find_if(
                meta.assets.begin(), meta.assets.end(),
                [&](const MetaAsset& existing) { return existing.guid == asset.guid; });
            if (duplicate != meta.assets.end()) {
                return Fail(context + ".guid", "duplicate asset GUID");
            }
            meta.assets.push_back(std::move(asset));
        }
    }

    if (const assets::JsonValue* dependencies = root.Find("dependencies");
        dependencies != nullptr) {
        if (!dependencies->IsArray()) {
            return Fail("dependencies", "expected an array");
        }
        for (std::size_t i = 0; i < dependencies->array_value.size(); ++i) {
            PARCEL_TRY_DECL(
                core::AssetGuid, guid,
                ReadGuid(dependencies->array_value[i], "dependencies[" + std::to_string(i) + "]"));
            const auto duplicate =
                std::find(meta.dependencies.begin(), meta.dependencies.end(), guid);
            if (duplicate == meta.dependencies.end()) {
                meta.dependencies.push_back(guid);
            }
        }
    }
    return meta;
}

std::string SerializeAssetMeta(const AssetMeta& meta, bool pretty) {
    assets::JsonValue root;
    root.type = assets::JsonType::Object;
    AddNumber(root, "version", static_cast<double>(meta.version));
    AddString(root, "sourceGuid", meta.source_guid.ToHex());
    AddString(root, "importer", meta.importer);
    AddNumber(root, "importerVersion", static_cast<double>(meta.importer_version));
    root.object_value.emplace_back("sourceHash", HashJson(meta.source_hash));
    root.object_value.emplace_back("settingsHash", HashJson(meta.settings_hash));
    assets::JsonValue settings = meta.settings;
    if (!settings.IsObject()) {
        settings.type = assets::JsonType::Object;
    }
    root.object_value.emplace_back("settings", std::move(settings));

    assets::JsonValue entries;
    entries.type = assets::JsonType::Array;
    entries.array_value.reserve(meta.assets.size());
    for (const MetaAsset& asset : meta.assets) {
        assets::JsonValue entry;
        entry.type = assets::JsonType::Object;
        AddString(entry, "guid", asset.guid.ToHex());
        if (!asset.type.empty()) {
            AddString(entry, "type", asset.type);
        }
        if (!asset.role.empty()) {
            AddString(entry, "role", asset.role);
        }
        if (!asset.local_id.empty()) {
            AddString(entry, "localId", asset.local_id);
        }
        entries.array_value.push_back(std::move(entry));
    }
    root.object_value.emplace_back("assets", std::move(entries));

    assets::JsonValue dependencies;
    dependencies.type = assets::JsonType::Array;
    dependencies.array_value.reserve(meta.dependencies.size());
    for (const core::AssetGuid& dependency : meta.dependencies) {
        assets::JsonValue value;
        value.type = assets::JsonType::String;
        value.string_value = dependency.ToHex();
        dependencies.array_value.push_back(std::move(value));
    }
    root.object_value.emplace_back("dependencies", std::move(dependencies));
    return assets::SerializeJson(root, pretty);
}

core::Result<AssetMeta> LoadAssetMetaFile(std::string_view path) {
    core::PlatformFile platform;
    auto file = platform.OpenRead(path);
    if (!file) {
        return std::move(file).TakeError();
    }
    auto bytes = core::ReadAll(*file.Value());
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.Value().data()),
                                bytes.Value().size());
    auto meta = ParseAssetMeta(text);
    if (!meta) {
        return std::move(meta).TakeError().WithPath(std::string(path));
    }
    return meta;
}

core::Result<void> WriteAssetMetaFile(std::string_view path, const AssetMeta& meta) {
    return WriteFile(path, SerializeAssetMeta(meta));
}

} // namespace parcel::import
