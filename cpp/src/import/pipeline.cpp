#include "parcel/import/pipeline.hpp"

#include "parcel/assets/asset_codec.hpp"
#include "parcel/assets/json.hpp"
#include "parcel/assets/serializers.hpp"
#include "parcel/core/error.hpp"
#include "parcel/core/path.hpp"
#include "parcel/core/platform_file.hpp"
#include "parcel/import/import_db.hpp"
#include "parcel/import/meta.hpp"
#include "shader_build.hpp"
#include "texture_canonical.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace parcel::import {
namespace {

using core::ErrorCode;
using core::MakeError;

struct SourceTask {
    std::string path;
    std::string relative;
    const IAssetImporter* importer = nullptr;
    bool force = false;
};

struct StagedAsset {
    core::AssetGuid guid;
    std::vector<std::byte> bytes;
    core::Hash256 hash;
};

struct SourceOutcome {
    std::string source;
    std::string relative;
    std::string status = "failed";
    std::string error;
    std::vector<std::string> warnings;
    AssetMeta meta;
    std::vector<StagedAsset> staged;
    ImportDBRecord record;
    bool meta_exists = false;
};

std::string LowerExtension(std::string_view path) {
    std::string extension(core::path::Extension(path));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

bool IsMetaFile(std::string_view path) {
    const std::string_view name = core::path::FileName(path);
    return name.ends_with(".parcelmeta") || name.ends_with(".parcel.json");
}

bool IsShaderManifest(std::string_view path) {
    return core::path::FileName(path) == "manifest.json";
}

bool IsShaderManifestPath(std::string_view path) {
    if (!IsShaderManifest(path)) {
        return false;
    }
    const std::string normalized = core::path::Normalize(path);
    const auto slash = normalized.find_last_of('/');
    const std::string parent =
        slash == std::string::npos ? std::string{} : normalized.substr(0, slash);
    return parent == "shaders" || parent.ends_with("/shaders");
}

std::string NormalizeAbsolute(std::string_view path) { return core::path::Normalize(path); }

std::filesystem::path Utf8Path(std::string_view path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

std::string DirectoryOf(std::string_view path) {
    const std::string normalized = core::path::Normalize(path);
    const auto slash = normalized.find_last_of('/');
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

core::Result<std::string> AbsolutePath(std::string_view path) {
    std::error_code error;
    const std::filesystem::path input = Utf8Path(path);
    const std::filesystem::path absolute = std::filesystem::absolute(input, error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to resolve absolute path: " + error.message())
            .WithPath(std::string(path));
    }
    const std::u8string value = absolute.u8string();
    return NormalizeAbsolute(
        std::string(reinterpret_cast<const char*>(value.data()), value.size()));
}

core::Result<std::string> RelativeTo(std::string_view root, std::string_view path) {
    std::error_code error;
    const std::filesystem::path root_path = Utf8Path(root);
    const std::filesystem::path path_value = Utf8Path(path);
    const std::filesystem::path relative = std::filesystem::relative(path_value, root_path, error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to make path relative: " + error.message())
            .WithPath(std::string(path));
    }
    const std::u8string value = relative.u8string();
    std::string result(reinterpret_cast<const char*>(value.data()), value.size());
    result = NormalizeAbsolute(result);
    if (result.rfind("../", 0) == 0 || result == "..") {
        return MakeError(ErrorCode::InvalidArgument, "source path escapes the project root")
            .WithPath(std::string(path));
    }
    return result;
}

core::Result<std::string> EnsureDirectory(std::string_view path) {
    if (path.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "directory path is empty");
    }
    core::PlatformFile platform;
    PARCEL_TRY(platform.CreateDirectories(path));
    return std::string(path);
}

core::Result<void> WriteBytesAtomic(std::string_view path, std::span<const std::byte> bytes) {
    core::PlatformFile platform;
    const std::string normalized = core::path::Normalize(path);
    const std::string temporary = normalized + ".tmp";
    PARCEL_TRY_DECL(std::unique_ptr<core::IFile>, file, platform.OpenWrite(temporary));
    PARCEL_TRY(core::WriteAll(*file, bytes));
    PARCEL_TRY(file->Flush());
    file->Close();
    return platform.Rename(temporary, normalized, true);
}

std::string FindSidecarPath(std::string_view source_path) {
    std::error_code error;
    const std::string direct = std::string(source_path) + ".parcel.json";
    if (std::filesystem::is_regular_file(
            std::filesystem::path(std::u8string(direct.begin(), direct.end())), error)) {
        return direct;
    }
    const std::string normalized = core::path::Normalize(source_path);
    const auto dot = normalized.find_last_of('.');
    const auto slash = normalized.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        const std::string alternate = normalized.substr(0, dot) + ".parcel.json";
        if (std::filesystem::is_regular_file(
                std::filesystem::path(std::u8string(alternate.begin(), alternate.end())), error)) {
            return alternate;
        }
    }
    return {};
}

core::Result<core::Hash256> HashSource(std::string_view path) {
    auto source = core::Sha256File(path);
    if (!source) {
        return std::move(source).TakeError();
    }
    const std::string sidecar = FindSidecarPath(path);
    if (sidecar.empty()) {
        return source.Value();
    }
    auto sidecar_hash = core::Sha256File(sidecar);
    if (!sidecar_hash) {
        return std::move(sidecar_hash).TakeError();
    }
    return core::Sha256(source.Value().ToHex() + ":" + sidecar_hash.Value().ToHex());
}

core::Result<bool> FileHash(std::string_view path, core::Hash256& out) {
    auto hash = core::Sha256File(path);
    if (!hash) {
        auto error = std::move(hash).TakeError();
        if (error.code == ErrorCode::NotFound) {
            return false;
        }
        return error;
    }
    out = hash.Value();
    return true;
}

core::Result<bool> OutputsCurrent(const ImportDBRecord& record, const ImportOptions& options) {
    if (record.outputs.empty()) {
        return false;
    }
    for (const ImportDBOutput& output : record.outputs) {
        auto path = core::path::Join(options.output_root, output.guid.ToHex() + ".prca");
        if (!path) {
            return std::move(path).TakeError();
        }
        core::Hash256 actual;
        auto present = FileHash(path.Value(), actual);
        if (!present) {
            return std::move(present).TakeError();
        }
        if (!present.Value() || actual != output.output_hash) {
            return false;
        }
    }
    return true;
}

core::Result<bool> DependencyOutputsCurrent(const ImportDBRecord& record, const ImportDB& db) {
    for (const ImportDBDependency& dependency : record.dependencies) {
        const ImportDBOutput* output = db.FindOutput(dependency.guid);
        if (output == nullptr || output->output_hash != dependency.output_hash) {
            return false;
        }
    }
    return true;
}

core::Result<std::vector<std::string>> DiscoverDirectory(const std::string& root,
                                                         const ImporterRegistry& importers) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(Utf8Path(root), error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to scan source directory: " + error.message())
            .WithPath(root);
    }

    std::vector<std::string> result;
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            return MakeError(ErrorCode::Io, "failed to scan source directory: " + error.message())
                .WithPath(root);
        }
        if (!iterator->is_regular_file(error)) {
            if (error) {
                error.clear();
            }
            continue;
        }
        const std::u8string value = iterator->path().u8string();
        const std::string path(reinterpret_cast<const char*>(value.data()), value.size());
        if (IsMetaFile(path)) {
            continue;
        }
        if (IsShaderManifest(path) && !IsShaderManifestPath(path)) {
            continue;
        }
        if (importers.FindForExtension(LowerExtension(path)) == nullptr) {
            continue;
        }
        result.push_back(NormalizeAbsolute(path));
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

core::Result<void> AddMetaAsset(SourceOutcome& outcome, MetaAsset asset) {
    const auto duplicate =
        std::find_if(outcome.meta.assets.begin(), outcome.meta.assets.end(),
                     [&](const MetaAsset& existing) { return existing.guid == asset.guid; });
    if (duplicate != outcome.meta.assets.end()) {
        return MakeError(ErrorCode::InvalidArgument, "duplicate asset GUID produced by importer");
    }
    outcome.meta.assets.push_back(std::move(asset));
    return {};
}

core::Result<SourceOutcome> ProcessSource(const SourceTask& task, const ImportOptions& options,
                                          const ImportDB& db,
                                          const assets::TypeRegistry& asset_registry,
                                          const TextureCanonicalIndex& texture_index,
                                          const core::Hash256& texture_index_hash) {
    SourceOutcome outcome;
    outcome.source = task.path;
    outcome.relative = task.relative;
    outcome.status = "failed";

    if (task.importer == nullptr) {
        outcome.error = "no importer supports this source";
        return outcome;
    }

    const std::string meta_path = AssetMetaPath(task.path);
    core::PlatformFile platform;
    auto meta_exists = platform.Exists(meta_path);
    if (!meta_exists) {
        return std::move(meta_exists).TakeError();
    }
    outcome.meta_exists = meta_exists.Value();
    if (outcome.meta_exists) {
        auto loaded = LoadAssetMetaFile(meta_path);
        if (!loaded) {
            return std::move(loaded).TakeError();
        }
        outcome.meta = std::move(loaded).Value();
    } else {
        outcome.meta.version = kAssetMetaVersion;
        outcome.meta.source_guid = MakeSourceGuid(task.relative);
        outcome.meta.settings = MakeDefaultImportSettings(task.importer->Name(), options.target);
    }
    if (outcome.meta.source_guid.IsZero()) {
        return MakeError(ErrorCode::InvalidArgument, "source GUID is zero");
    }
    outcome.meta.importer = std::string(task.importer->Name());
    outcome.meta.importer_version = task.importer->Version();

    auto source_hash = HashSource(task.path);
    if (!source_hash) {
        return std::move(source_hash).TakeError();
    }
    outcome.meta.source_hash = source_hash.Value();
    outcome.meta.settings_hash = HashSettings(outcome.meta.settings);

    const ImportDBRecord* existing = db.FindSource(task.relative);
    if (!options.force && !task.force && outcome.meta_exists && existing != nullptr &&
        existing->source_guid == outcome.meta.source_guid &&
        existing->importer == outcome.meta.importer &&
        existing->importer_version == outcome.meta.importer_version &&
        existing->source_hash == outcome.meta.source_hash &&
        existing->settings_hash == outcome.meta.settings_hash &&
        ((task.importer->Name() != "fbx" && task.importer->Name() != "gltf") ||
         existing->texture_index_hash == texture_index_hash)) {
        auto current = OutputsCurrent(*existing, options);
        if (!current) {
            return std::move(current).TakeError();
        }
        auto deps_current = DependencyOutputsCurrent(*existing, db);
        if (!deps_current) {
            return std::move(deps_current).TakeError();
        }
        if (current.Value() && deps_current.Value()) {
            outcome.status = "skipped";
            outcome.record = *existing;
            outcome.record.texture_index_hash = texture_index_hash;
            return outcome;
        }
    }

    ImportRequest request;
    request.project_root = options.project_root;
    request.source_path = task.path;
    request.project_relative_path = task.relative;
    request.source_guid = outcome.meta.source_guid;
    request.settings = &outcome.meta.settings;
    request.texture_index = &texture_index;
    auto imported = task.importer->Import(request);
    if (!imported) {
        return std::move(imported).TakeError();
    }
    if (imported.Value().assets.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "importer produced no assets");
    }
    for (const std::string& warning : imported.Value().warnings) {
        outcome.warnings.push_back(warning);
    }

    const std::vector<MetaAsset> old_assets = outcome.meta.assets;
    outcome.meta.assets.clear();
    outcome.meta.dependencies.clear();
    std::unordered_set<core::AssetGuid, core::Guid128Hash> used_guids;
    for (std::size_t index = 0; index < imported.Value().assets.size(); ++index) {
        CookedAsset& cooked = imported.Value().assets[index];
        if (!cooked.object) {
            return MakeError(ErrorCode::Internal, "importer produced a null asset");
        }
        const assets::ITypeSerializer* serializer = asset_registry.Find(cooked.object->Type());
        if (serializer == nullptr) {
            return MakeError(ErrorCode::Unsupported, "asset type is not registered");
        }

        core::AssetGuid guid = cooked.guid;
        if (guid.IsZero()) {
            const auto old =
                std::find_if(old_assets.begin(), old_assets.end(), [&](const MetaAsset& asset) {
                    return !cooked.local_id.empty() && asset.local_id == cooked.local_id;
                });
            if (old != old_assets.end()) {
                guid = old->guid;
            }
        }
        if (guid.IsZero()) {
            std::string local_id = cooked.local_id;
            if (local_id.empty()) {
                local_id =
                    (cooked.role.empty() ? "asset" : cooked.role) + ":" + std::to_string(index);
            }
            guid = MakeSubAssetGuid(outcome.meta.source_guid, cooked.role, local_id);
        }
        if (!used_guids.insert(guid).second) {
            return MakeError(ErrorCode::InvalidArgument, "importer produced duplicate asset GUID");
        }

        auto bytes = assets::EncodeAssetBytes(*cooked.object, asset_registry);
        if (!bytes) {
            return std::move(bytes).TakeError();
        }
        std::vector<core::AssetGuid> asset_references;
        if (auto collected = serializer->CollectReferences(*cooked.object, asset_references);
            !collected) {
            return std::move(collected).TakeError();
        }
        for (const core::AssetGuid& reference : asset_references) {
            if (!reference.IsZero() &&
                std::find(outcome.meta.dependencies.begin(), outcome.meta.dependencies.end(),
                          reference) == outcome.meta.dependencies.end()) {
                outcome.meta.dependencies.push_back(reference);
            }
        }
        StagedAsset staged;
        staged.guid = guid;
        staged.bytes = std::move(bytes).Value();
        staged.hash = core::Sha256(staged.bytes);
        outcome.staged.push_back(std::move(staged));

        MetaAsset meta_asset;
        meta_asset.guid = guid;
        meta_asset.type = std::string(serializer->TypeName());
        meta_asset.role = cooked.role;
        meta_asset.local_id = cooked.local_id;
        PARCEL_TRY(AddMetaAsset(outcome, std::move(meta_asset)));

        for (const core::AssetGuid& dependency : cooked.dependencies) {
            if (!dependency.IsZero() &&
                std::find(outcome.meta.dependencies.begin(), outcome.meta.dependencies.end(),
                          dependency) == outcome.meta.dependencies.end()) {
                outcome.meta.dependencies.push_back(dependency);
            }
        }
    }
    for (const core::AssetGuid& dependency : imported.Value().dependencies) {
        if (!dependency.IsZero() &&
            std::find(outcome.meta.dependencies.begin(), outcome.meta.dependencies.end(),
                      dependency) == outcome.meta.dependencies.end()) {
            outcome.meta.dependencies.push_back(dependency);
        }
    }

    outcome.record.source_path = task.relative;
    outcome.record.source_guid = outcome.meta.source_guid;
    outcome.record.importer = outcome.meta.importer;
    outcome.record.importer_version = outcome.meta.importer_version;
    outcome.record.source_hash = outcome.meta.source_hash;
    outcome.record.settings_hash = outcome.meta.settings_hash;
    outcome.record.texture_index_hash = texture_index_hash;
    for (const StagedAsset& staged : outcome.staged) {
        outcome.record.outputs.push_back({staged.guid, staged.hash});
    }
    for (const core::AssetGuid& dependency : outcome.meta.dependencies) {
        outcome.record.dependencies.push_back({dependency, core::Hash256{}});
    }
    outcome.status = "imported";
    return outcome;
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

core::Result<void> WriteReport(const ImportOptions& options, const ImportSummary& summary) {
    assets::JsonValue root;
    root.type = assets::JsonType::Object;
    AddNumber(root, "version", 1);
    AddString(root, "target", options.target);
    AddNumber(root, "imported", summary.imported);
    AddNumber(root, "skipped", summary.skipped);
    AddNumber(root, "failed", summary.failed);
    assets::JsonValue entries;
    entries.type = assets::JsonType::Array;
    for (const ImportReportEntry& entry : summary.entries) {
        assets::JsonValue object;
        object.type = assets::JsonType::Object;
        AddString(object, "source", entry.source);
        AddString(object, "importer", entry.importer);
        AddNumber(object, "importerVersion", entry.importer_version);
        AddString(object, "settingsHash", entry.settings_hash);
        AddString(object, "status", entry.status);
        if (!entry.error.empty()) {
            AddString(object, "error", entry.error);
        }
        assets::JsonValue assets_value;
        assets_value.type = assets::JsonType::Array;
        for (const std::string& asset : entry.assets) {
            assets::JsonValue value;
            value.type = assets::JsonType::String;
            value.string_value = asset;
            assets_value.array_value.push_back(std::move(value));
        }
        object.object_value.emplace_back("assets", std::move(assets_value));
        assets::JsonValue warnings;
        warnings.type = assets::JsonType::Array;
        for (const std::string& warning : entry.warnings) {
            assets::JsonValue value;
            value.type = assets::JsonType::String;
            value.string_value = warning;
            warnings.array_value.push_back(std::move(value));
        }
        object.object_value.emplace_back("warnings", std::move(warnings));
        entries.array_value.push_back(std::move(object));
    }
    root.object_value.emplace_back("entries", std::move(entries));

    const std::string text = assets::SerializeJson(root, true);
    auto output_directory = EnsureDirectory(options.output_root);
    if (!output_directory) {
        return std::move(output_directory).TakeError();
    }
    auto report_path = core::path::Join(options.output_root, "import_report.json");
    if (!report_path) {
        return std::move(report_path).TakeError();
    }
    return WriteBytesAtomic(report_path.Value(),
                            std::as_bytes(std::span(text.data(), text.size())));
}

} // namespace

core::Result<std::vector<std::string>>
DiscoverImportSources(const ImportOptions& options, std::span<const std::string> explicit_sources,
                      const ImporterRegistry& importers) {
    if (!explicit_sources.empty()) {
        std::vector<std::string> result;
        result.reserve(explicit_sources.size());
        for (const std::string& source : explicit_sources) {
            auto absolute = AbsolutePath(source);
            if (!absolute) {
                return std::move(absolute).TakeError();
            }
            result.push_back(std::move(absolute).Value());
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    auto root = AbsolutePath(options.source_root);
    if (!root) {
        return std::move(root).TakeError();
    }
    core::PlatformFile platform;
    auto stat = platform.Stat(root.Value());
    if (!stat) {
        return std::move(stat).TakeError();
    }
    if (stat.Value().is_directory) {
        return DiscoverDirectory(root.Value(), importers);
    }
    return std::vector<std::string>{std::move(root).Value()};
}

core::Result<ImportSummary> RunImport(const ImportOptions& options,
                                      std::span<const std::string> explicit_sources,
                                      const ImporterRegistry& importers) {
    const auto started = std::chrono::steady_clock::now();
    if (options.project_root.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "project root is empty");
    }
    if (options.source_root.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "source root is empty");
    }
    if (options.output_root.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "output root is empty");
    }
    if (options.jobs == 0) {
        return MakeError(ErrorCode::InvalidArgument, "jobs must be greater than zero");
    }

    auto project_root = AbsolutePath(options.project_root);
    if (!project_root) {
        return std::move(project_root).TakeError();
    }
    auto source_root = AbsolutePath(options.source_root);
    if (!source_root) {
        return std::move(source_root).TakeError();
    }
    auto output_root = AbsolutePath(options.output_root);
    if (!output_root) {
        return std::move(output_root).TakeError();
    }

    ImportOptions normalized = options;
    normalized.project_root = project_root.Value();
    normalized.source_root = source_root.Value();
    normalized.output_root = output_root.Value();

    bool shader_build_ran = false;
    std::string built_shader_manifest;
    if (normalized.shader_build && !normalized.dry_run) {
        auto built = detail::RunShaderBuild(*normalized.shader_build, normalized.project_root,
                                            normalized.shader_manifest);
        if (!built) {
            return std::move(built).TakeError();
        }
        shader_build_ran = true;
        built_shader_manifest = std::move(built).Value().manifest_path;
    }

    auto sources = DiscoverImportSources(normalized, explicit_sources, importers);
    if (!sources) {
        return std::move(sources).TakeError();
    }
    auto texture_index = detail::BuildTextureCanonicalIndex(
        normalized.project_root, normalized.target, sources.Value(), importers);
    if (!texture_index) {
        return std::move(texture_index).TakeError();
    }
    const core::Hash256 texture_index_hash = HashTextureCanonicalIndex(texture_index.Value());

    std::string shader_manifest;
    if (!options.shader_manifest.empty()) {
        auto absolute_manifest = AbsolutePath(options.shader_manifest);
        if (!absolute_manifest) {
            return std::move(absolute_manifest).TakeError();
        }
        shader_manifest = std::move(absolute_manifest).Value();
    } else if (!built_shader_manifest.empty()) {
        shader_manifest = built_shader_manifest;
    } else {
        const auto found =
            std::find_if(sources.Value().begin(), sources.Value().end(),
                         [](const std::string& source) { return IsShaderManifestPath(source); });
        if (found != sources.Value().end()) {
            shader_manifest = *found;
        }
    }
    const std::string shader_root =
        shader_manifest.empty() ? std::string{} : DirectoryOf(shader_manifest);
    std::vector<SourceTask> tasks;
    tasks.reserve(sources.Value().size() + (shader_manifest.empty() ? 0 : 1));
    const bool output_inside_source =
        core::path::IsWithin(normalized.source_root, normalized.output_root);
    for (const std::string& source : sources.Value()) {
        if (output_inside_source && core::path::IsWithin(normalized.output_root, source)) {
            continue;
        }
        if (!shader_root.empty() && source != shader_manifest &&
            core::path::IsWithin(shader_root, source)) {
            continue;
        }
        SourceTask task;
        task.path = source;
        auto relative = RelativeTo(normalized.project_root, source);
        if (!relative) {
            return std::move(relative).TakeError();
        }
        task.relative = relative.Value();
        if (!shader_manifest.empty() && task.path == shader_manifest) {
            task.importer = importers.FindByName("shader");
            task.force = shader_build_ran;
        } else if (IsShaderManifest(task.path)) {
            continue;
        } else {
            task.importer = importers.FindForExtension(LowerExtension(task.path));
        }
        tasks.push_back(std::move(task));
    }
    if (!shader_manifest.empty() &&
        std::none_of(tasks.begin(), tasks.end(),
                     [&](const SourceTask& task) { return task.path == shader_manifest; })) {
        SourceTask task;
        task.path = shader_manifest;
        auto relative = RelativeTo(normalized.project_root, shader_manifest);
        if (!relative) {
            return std::move(relative).TakeError();
        }
        task.relative = relative.Value();
        task.importer = importers.FindByName("shader");
        task.force = shader_build_ran;
        tasks.push_back(std::move(task));
    }

    std::string db_path;
    if (options.db_path.empty()) {
        auto default_path = core::path::Join(normalized.output_root, "import.db");
        if (!default_path) {
            return std::move(default_path).TakeError();
        }
        db_path = std::move(default_path).Value();
    } else {
        auto absolute_db = AbsolutePath(options.db_path);
        if (!absolute_db) {
            return std::move(absolute_db).TakeError();
        }
        db_path = std::move(absolute_db).Value();
    }
    auto db = LoadImportDB(db_path);
    if (!db) {
        return std::move(db).TakeError();
    }

    assets::TypeRegistry asset_registry;
    if (auto registered = assets::RegisterStandardAssetSerializers(asset_registry); !registered) {
        return std::move(registered).TakeError();
    }

    std::vector<SourceOutcome> outcomes(tasks.size());
    std::atomic_size_t next_task{0};
    const std::size_t worker_count =
        std::min<std::size_t>(std::max<std::uint32_t>(1, normalized.jobs), tasks.size());
    auto worker = [&]() {
        for (;;) {
            const std::size_t index = next_task.fetch_add(1);
            if (index >= tasks.size()) {
                return;
            }
            auto result = ProcessSource(tasks[index], normalized, db.Value(), asset_registry,
                                        texture_index.Value(), texture_index_hash);
            if (!result) {
                SourceOutcome failed;
                failed.source = tasks[index].path;
                failed.relative = tasks[index].relative;
                failed.error = result.GetError().ToString();
                outcomes[index] = std::move(failed);
            } else {
                outcomes[index] = std::move(result).Value();
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

    ImportSummary summary;
    std::unordered_map<core::AssetGuid, core::Hash256, core::Guid128Hash> output_hashes;
    for (const ImportDBRecord& record : db.Value().records) {
        for (const ImportDBOutput& output : record.outputs) {
            output_hashes[output.guid] = output.output_hash;
        }
    }

    std::vector<ImportDBRecord> next_records;
    next_records.reserve(outcomes.size());
    for (const SourceOutcome& outcome : outcomes) {
        if (outcome.status == "skipped") {
            ImportDBRecord record = outcome.record;
            next_records.push_back(std::move(record));
            summary.skipped++;
        } else if (outcome.status == "imported") {
            ImportDBRecord record = outcome.record;
            next_records.push_back(std::move(record));
            summary.imported++;
            for (const StagedAsset& asset : outcome.staged) {
                output_hashes[asset.guid] = asset.hash;
            }
        } else {
            summary.failed++;
        }
    }
    for (ImportDBRecord& record : next_records) {
        for (ImportDBDependency& dependency : record.dependencies) {
            const auto found = output_hashes.find(dependency.guid);
            if (found != output_hashes.end()) {
                dependency.output_hash = found->second;
            }
        }
    }

    std::unordered_map<core::AssetGuid, core::Hash256, core::Guid128Hash> output_guids;
    for (const SourceOutcome& outcome : outcomes) {
        if (outcome.status != "imported") {
            continue;
        }
        for (const StagedAsset& asset : outcome.staged) {
            const auto [existing, inserted] = output_guids.emplace(asset.guid, asset.hash);
            if (!inserted && existing->second != asset.hash) {
                return MakeError(ErrorCode::InvalidArgument,
                                 "multiple sources produced the same asset GUID with "
                                 "different content: " +
                                     asset.guid.ToHex());
            }
        }
    }

    if (!normalized.dry_run) {
        auto output_directory = EnsureDirectory(normalized.output_root);
        if (!output_directory) {
            return std::move(output_directory).TakeError();
        }
        std::unordered_set<core::AssetGuid, core::Guid128Hash> written_guids;
        for (const SourceOutcome& outcome : outcomes) {
            if (outcome.status != "imported") {
                continue;
            }
            for (const StagedAsset& asset : outcome.staged) {
                if (!written_guids.insert(asset.guid).second) {
                    continue;
                }
                auto path = core::path::Join(normalized.output_root, asset.guid.ToHex() + ".prca");
                if (!path) {
                    return std::move(path).TakeError();
                }
                if (auto written = WriteBytesAtomic(path.Value(), asset.bytes); !written) {
                    return std::move(written).TakeError();
                }
            }
            auto meta_written = WriteAssetMetaFile(AssetMetaPath(outcome.source), outcome.meta);
            if (!meta_written) {
                return std::move(meta_written).TakeError();
            }
        }
        if (auto saved = SaveImportDB(db_path, next_records); !saved) {
            return std::move(saved).TakeError();
        }
    }

    for (const SourceOutcome& outcome : outcomes) {
        ImportReportEntry entry;
        entry.source = outcome.relative;
        entry.importer = outcome.meta.importer;
        entry.importer_version = outcome.meta.importer_version;
        entry.settings_hash = outcome.meta.settings_hash.ToHex();
        entry.status = outcome.status;
        entry.error = outcome.error;
        entry.warnings = outcome.warnings;
        for (const StagedAsset& asset : outcome.staged) {
            entry.assets.push_back(asset.guid.ToHex());
        }
        if (entry.status == "skipped") {
            auto found = db.Value().FindSource(outcome.relative);
            if (found != nullptr) {
                for (const ImportDBOutput& output : found->outputs) {
                    entry.assets.push_back(output.guid.ToHex());
                }
            }
        }
        summary.entries.push_back(std::move(entry));
    }
    summary.elapsed_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count());

    if (!normalized.dry_run) {
        if (auto report = WriteReport(normalized, summary); !report) {
            return std::move(report).TakeError();
        }
    }
    return summary;
}

core::Result<ImportSummary> RunImport(const ImportOptions& options,
                                      const ImporterRegistry& importers) {
    return RunImport(options, std::span<const std::string>{}, importers);
}

} // namespace parcel::import
