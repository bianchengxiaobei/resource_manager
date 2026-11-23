#include "parcel/assets/shader_library_asset.hpp"
#include "parcel/core/error.hpp"
#include "parcel/core/path.hpp"
#include "parcel/core/platform_file.hpp"
#include "parcel/import/importer.hpp"
#include "parcel/import/meta.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace parcel::import {
namespace {

using assets::ShaderBackend;
using assets::ShaderBackendLibrary;
using assets::ShaderLibraryAsset;
using assets::ShaderPassBinary;
using assets::ShaderStage;
using core::ErrorCode;
using core::MakeError;

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

core::Result<std::vector<std::byte>> ReadFile(std::string_view path) {
    core::PlatformFile platform;
    auto file = platform.OpenRead(path);
    if (!file) {
        return std::move(file).TakeError();
    }
    return core::ReadAll(*file.Value());
}

core::Result<ShaderBackend> ParseBackend(std::string_view name) {
    if (name == "dxbc") {
        return ShaderBackend::Dxbc;
    }
    if (name == "spirv") {
        return ShaderBackend::Spirv;
    }
    return MakeError(ErrorCode::Unsupported, "unsupported shader backend: " + std::string(name));
}

std::string StageName(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:
        return "vertex";
    case ShaderStage::Fragment:
        return "fragment";
    case ShaderStage::BufferFragment:
        return "bufferFragment";
    case ShaderStage::Compute:
        return "compute";
    case ShaderStage::BufferCompute:
        return "bufferCompute";
    }
    return "unknown";
}

class ShaderImporter final : public IAssetImporter {
  public:
    std::string_view Name() const noexcept override { return "shader"; }
    std::uint32_t Version() const noexcept override { return 1; }

    bool Supports(std::string_view extension) const noexcept override {
        return extension == ".shadermanifest";
    }

    core::Result<ImportResult> Import(const ImportRequest& request) const override {
        auto manifest_bytes = ReadFile(request.source_path);
        if (!manifest_bytes) {
            return std::move(manifest_bytes).TakeError();
        }
        const std::string_view text(reinterpret_cast<const char*>(manifest_bytes.Value().data()),
                                    manifest_bytes.Value().size());
        auto parsed = assets::ParseJson(text);
        if (!parsed) {
            return std::move(parsed).TakeError().WithPath(request.source_path);
        }
        const assets::JsonValue& root = parsed.Value();
        if (!root.IsObject()) {
            return MakeError(ErrorCode::InvalidArgument, "shader manifest root must be an object");
        }
        const assets::JsonValue* version = root.Find("version");
        if (version == nullptr || !version->IsNumber() || version->number_value != 1.0) {
            return MakeError(ErrorCode::Unsupported, "unsupported shader manifest version");
        }
        const assets::JsonValue* passes = root.Find("passes");
        if (passes == nullptr || !passes->IsObject()) {
            return MakeError(ErrorCode::InvalidArgument, "shader manifest has no passes object");
        }

        const std::string manifest_directory = DirectoryOf(request.source_path);
        auto asset = std::make_shared<ShaderLibraryAsset>();
        asset->name = std::string(core::path::Stem(request.source_path));
        const std::string_view library_name = asset->name;
        if (request.settings != nullptr) {
            asset->pipeline_key = SettingString(*request.settings, "pipelineKey", "hdrp");
            asset->material_variant = SettingString(*request.settings, "materialVariant", "");
        }

        for (const auto& [backend_name, backend_value] : passes->object_value) {
            auto backend = ParseBackend(backend_name);
            if (!backend) {
                return std::move(backend).TakeError().WithPath(request.source_path);
            }
            if (!backend_value.IsObject()) {
                return MakeError(ErrorCode::InvalidArgument,
                                 "shader backend must be an object: " + backend_name);
            }
            ShaderBackendLibrary library;
            library.backend = backend.Value();
            std::unordered_set<std::string> variant_names;
            for (const auto& [pass_name, pass_value] : backend_value.object_value) {
                if (pass_name.empty()) {
                    return MakeError(ErrorCode::InvalidArgument, "shader pass name is empty");
                }
                if (!pass_value.IsObject()) {
                    return MakeError(ErrorCode::InvalidArgument,
                                     "shader pass must be an object: " + pass_name);
                }
                const std::pair<std::string_view, ShaderStage> single_fields[] = {
                    {"vertex", ShaderStage::Vertex},
                    {"fragment", ShaderStage::Fragment},
                    {"compute", ShaderStage::Compute},
                    {"bufferCompute", ShaderStage::BufferCompute},
                };
                for (const auto& [field, stage] : single_fields) {
                    const assets::JsonValue* path_value = pass_value.Find(field);
                    if (path_value == nullptr) {
                        continue;
                    }
                    if (!path_value->IsString() || path_value->string_value.empty()) {
                        return MakeError(ErrorCode::InvalidArgument,
                                         "shader pass path must be a non-empty string: " +
                                             pass_name + "." + std::string(field));
                    }
                    PARCEL_TRY(AddBinary(library_name, library, manifest_directory, pass_name,
                                         stage, path_value->string_value, variant_names));
                }
                const std::pair<std::string_view, ShaderStage> array_fields[] = {
                    {"fragments", ShaderStage::Fragment},
                    {"bufferFragments", ShaderStage::BufferFragment},
                    {"bufferComputes", ShaderStage::BufferCompute},
                };
                for (const auto& [field, stage] : array_fields) {
                    const assets::JsonValue* entries = pass_value.Find(field);
                    if (entries == nullptr) {
                        continue;
                    }
                    if (!entries->IsArray()) {
                        return MakeError(ErrorCode::InvalidArgument,
                                         "shader pass field must be an array: " + pass_name + "." +
                                             std::string(field));
                    }
                    for (std::size_t i = 0; i < entries->array_value.size(); ++i) {
                        const assets::JsonValue& path_value = entries->array_value[i];
                        if (!path_value.IsString() || path_value.string_value.empty()) {
                            return MakeError(
                                ErrorCode::InvalidArgument,
                                "shader pass array entry must be a non-empty string: " + pass_name +
                                    "." + std::string(field) + "[" + std::to_string(i) + "]");
                        }
                        PARCEL_TRY(AddBinary(library_name, library, manifest_directory, pass_name,
                                             stage, path_value.string_value, variant_names));
                    }
                }
            }
            asset->backends.push_back(std::move(library));
        }
        if (asset->backends.empty()) {
            return MakeError(ErrorCode::InvalidArgument, "shader manifest contains no backends");
        }
        std::sort(asset->backends.begin(), asset->backends.end(),
                  [](const ShaderBackendLibrary& lhs, const ShaderBackendLibrary& rhs) {
                      return static_cast<int>(lhs.backend) < static_cast<int>(rhs.backend);
                  });

        ImportResult result;
        CookedAsset cooked;
        cooked.local_id = "shader";
        cooked.role = "shader";
        cooked.object = std::move(asset);
        result.assets.push_back(std::move(cooked));
        return result;
    }

  private:
    static core::Result<void>
    AddBinary(std::string_view library_name, ShaderBackendLibrary& backend,
              std::string_view manifest_directory, std::string_view pass_name, ShaderStage stage,
              std::string_view relative_path, std::unordered_set<std::string>& variant_names) {
        if (!core::path::IsSafeRelative(relative_path)) {
            return MakeError(ErrorCode::InvalidArgument,
                             "shader path must be a safe relative path: " +
                                 std::string(relative_path));
        }
        auto path = core::path::ResolveWithin(manifest_directory, relative_path);
        if (!path) {
            return std::move(path).TakeError();
        }
        auto bytes = ReadFile(path.Value());
        if (!bytes) {
            return std::move(bytes).TakeError().WithPath(path.Value());
        }
        std::string variant = std::string(pass_name) + "." + StageName(stage) + "." +
                              std::to_string(backend.passes.size());
        if (!variant_names.insert(variant).second) {
            return MakeError(ErrorCode::InvalidArgument, "duplicate shader variant: " + variant +
                                                             " in " + std::string(library_name));
        }
        ShaderPassBinary pass;
        pass.pass = std::string(pass_name);
        pass.stage = stage;
        pass.data = std::move(bytes).Value();
        backend.passes.push_back(std::move(pass));
        backend.variants.push_back(std::move(variant));
        return {};
    }
};

} // namespace

std::shared_ptr<const IAssetImporter> MakeShaderImporter() {
    return std::make_shared<ShaderImporter>();
}

} // namespace parcel::import
