#include "parcel/hbg/shader_library_bootstrap.hpp"

#include "parcel/core/error.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/path.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace parcel_hbg {
namespace {

using parcel::assets::ShaderBackend;
using parcel::assets::ShaderBackendLibrary;
using parcel::assets::ShaderPassBinary;
using parcel::assets::ShaderStage;
using parcel::core::ErrorCode;
using parcel::core::MakeError;

struct PassFiles {
    std::string vertex;
    std::vector<std::string> fragments;
    std::vector<std::string> buffer_fragments;
    std::string compute;
    std::string buffer_compute;
    std::vector<std::string> computes;
    std::vector<std::string> buffer_computes;
};

void AppendU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (std::uint32_t index = 0; index < 4; ++index) {
        out.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
    }
}

void AppendU64(std::vector<std::byte>& out, std::uint64_t value) {
    for (std::uint32_t index = 0; index < 8; ++index) {
        out.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
    }
}

void AppendString(std::vector<std::byte>& out, std::string_view value) {
    AppendU32(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), reinterpret_cast<const std::byte*>(value.data()),
               reinterpret_cast<const std::byte*>(value.data()) + value.size());
}

std::string BackendName(ShaderBackend backend) {
    switch (backend) {
    case ShaderBackend::Dxbc:
        return "dxbc";
    case ShaderBackend::Spirv:
        return "spirv";
    }
    return {};
}

std::string StageSuffix(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:
        return "vs";
    case ShaderStage::Fragment:
        return "fs";
    case ShaderStage::BufferFragment:
        return "bfs";
    case ShaderStage::Compute:
        return "cs";
    case ShaderStage::BufferCompute:
        return "bcs";
    }
    return {};
}

std::string JsonEscape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result.push_back(character);
            break;
        }
    }
    return result;
}

bool ValidPassName(std::string_view name) {
    return parcel::core::path::IsSafeRelative(name) && name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos && name != ".";
}

std::string JoinPath(const std::filesystem::path& root, std::string_view relative) {
    std::filesystem::path result = root;
    std::string value(relative);
    std::size_t start = 0;
    while (start < value.size()) {
        const std::size_t slash = value.find('/', start);
        const std::string_view segment(value.data() + start,
                                       (slash == std::string::npos ? value.size() : slash) - start);
        result /= std::string(segment);
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return result.string();
}

void AppendArray(std::string& json, const std::vector<std::string>& values) {
    json.push_back('[');
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            json.push_back(',');
        }
        json += '"';
        json += JsonEscape(values[index]);
        json += '"';
    }
    json.push_back(']');
}

Result<void> WriteBinary(const std::filesystem::path& path, std::span<const std::byte> data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return MakeError(ErrorCode::Io, "failed to open shader binary for writing");
    }
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    if (!file) {
        return MakeError(ErrorCode::Io, "failed to write shader binary");
    }
    return {};
}

constexpr std::string_view kBlobDirectoryName = ".blobs";
std::atomic<std::uint64_t> g_blob_temp_sequence{0};

std::string TempSuffix() {
    static const std::uint64_t process_nonce = [] {
        std::random_device random;
        const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const auto address = reinterpret_cast<std::uintptr_t>(&g_blob_temp_sequence);
        return (static_cast<std::uint64_t>(random()) << 32U) ^
               static_cast<std::uint64_t>(random()) ^ static_cast<std::uint64_t>(now) ^
               static_cast<std::uint64_t>(address);
    }();
    return ".tmp." + std::to_string(process_nonce) + "." +
           std::to_string(g_blob_temp_sequence.fetch_add(1));
}

Result<void> WriteText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::path temporary = path;
    temporary += TempSuffix();
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return MakeError(ErrorCode::Io, "failed to open shader manifest for writing");
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return MakeError(ErrorCode::Io, "failed to write shader manifest");
        }
    }

    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (!error) {
        return {};
    }
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    if (!size_error && size == text.size()) {
        return {};
    }
    return MakeError(ErrorCode::Io, "failed to commit shader manifest");
}

Result<std::filesystem::path> EnsureBlob(const std::filesystem::path& blobs,
                                         std::span<const std::byte> data) {
    const std::filesystem::path blob = blobs / (parcel::core::Sha256(data).ToHex() + ".bin");
    std::error_code error;
    if (std::filesystem::exists(blob, error)) {
        if (error) {
            return MakeError(ErrorCode::Io, "failed to inspect shader blob");
        }
        const auto size = std::filesystem::file_size(blob, error);
        if (error) {
            return MakeError(ErrorCode::Io, "failed to inspect shader blob size");
        }
        if (size == data.size()) {
            return blob;
        }
        std::filesystem::remove(blob, error);
        if (error) {
            return MakeError(ErrorCode::Io, "failed to replace shader blob");
        }
    } else if (error) {
        return MakeError(ErrorCode::Io, "failed to inspect shader blob");
    }

    std::filesystem::path temporary = blob;
    temporary += TempSuffix();
    auto written = WriteBinary(temporary, data);
    if (!written) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return std::move(written).TakeError();
    }
    std::filesystem::rename(temporary, blob, error);
    if (error) {
        std::error_code size_error;
        const auto size = std::filesystem::file_size(blob, size_error);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (!size_error && size == data.size()) {
            return blob;
        }
        return MakeError(ErrorCode::Io, "failed to commit shader blob");
    }
    return blob;
}

Result<void> MaterializeBlob(const std::filesystem::path& blob, const std::filesystem::path& output,
                             bool use_hard_links) {
    std::error_code error;
    const auto blob_size = std::filesystem::file_size(blob, error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to inspect shader blob size");
    }
    const auto output_ready = [&]() {
        std::error_code equivalent_error;
        if (std::filesystem::equivalent(blob, output, equivalent_error) && !equivalent_error) {
            return true;
        }
        std::error_code size_error;
        const auto size = std::filesystem::file_size(output, size_error);
        return !size_error && size == blob_size;
    };

    if (std::filesystem::exists(output, error)) {
        if (error) {
            return MakeError(ErrorCode::Io, "failed to inspect shader binary");
        }
        if (output_ready()) {
            return {};
        }
        std::filesystem::remove(output, error);
        if (error && !output_ready()) {
            return MakeError(ErrorCode::Io, "failed to replace shader binary");
        }
    } else if (error) {
        return MakeError(ErrorCode::Io, "failed to inspect shader binary");
    }

    if (use_hard_links) {
        std::filesystem::create_hard_link(blob, output, error);
        if (!error || output_ready()) {
            return {};
        }
    }

    std::filesystem::path temporary = output;
    temporary += TempSuffix();
    std::filesystem::copy_file(blob, temporary, std::filesystem::copy_options::overwrite_existing,
                               error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (output_ready()) {
            return {};
        }
        return MakeError(ErrorCode::Io, "failed to materialize shader binary");
    }
    std::filesystem::rename(temporary, output, error);
    if (!error) {
        return {};
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    if (output_ready()) {
        return {};
    }
    return MakeError(ErrorCode::Io, "failed to materialize shader binary");
}

void CleanupOrphanBlobs(const std::filesystem::path& blobs) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(blobs, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        const std::filesystem::directory_entry entry = *iterator;
        iterator.increment(error);
        if (error) {
            return;
        }
        std::error_code type_error;
        if (!entry.is_regular_file(type_error) || type_error) {
            continue;
        }
        if (entry.path().filename().string().find(".tmp.") != std::string::npos) {
            continue;
        }
        std::error_code count_error;
        const auto links = std::filesystem::hard_link_count(entry.path(), count_error);
        if (count_error || links > 1) {
            continue;
        }
        std::error_code remove_error;
        std::filesystem::remove(entry.path(), remove_error);
    }
}

Result<std::vector<std::byte>> CanonicalBytes(const parcel::assets::ShaderLibraryAsset& asset) {
    std::vector<std::byte> bytes;
    AppendString(bytes, "parcel-shader-library-v1");
    AppendString(bytes, asset.name);
    AppendString(bytes, asset.material_variant);
    AppendString(bytes, asset.pipeline_key);
    AppendU32(bytes, static_cast<std::uint32_t>(asset.backends.size()));
    for (const ShaderBackendLibrary& backend : asset.backends) {
        AppendU32(bytes, static_cast<std::uint32_t>(backend.backend));
        AppendU32(bytes, static_cast<std::uint32_t>(backend.variants.size()));
        for (const std::string& variant : backend.variants) {
            AppendString(bytes, variant);
        }
        AppendU32(bytes, static_cast<std::uint32_t>(backend.passes.size()));
        for (const ShaderPassBinary& pass : backend.passes) {
            AppendString(bytes, pass.pass);
            AppendU32(bytes, static_cast<std::uint32_t>(pass.stage));
            AppendU64(bytes, pass.blob_id);
            AppendU64(bytes, pass.data.size());
            bytes.insert(bytes.end(), pass.data.begin(), pass.data.end());
        }
    }
    return bytes;
}

} // namespace

Result<std::string>
ShaderLibraryBootstrap::ContentHash(const parcel::assets::ShaderLibraryAsset& asset) {
    auto bytes = CanonicalBytes(asset);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    return parcel::core::Sha256(bytes.Value()).ToHex();
}

Result<ShaderMaterialization>
ShaderLibraryBootstrap::Materialize(const parcel::assets::ShaderLibraryAsset& asset,
                                    const ShaderMaterializeOptions& options) {
    if (options.cache_root.empty() || options.package.empty() ||
        !parcel::core::path::IsSafeRelative(options.package)) {
        return MakeError(ErrorCode::InvalidArgument, "shader materialization options are invalid");
    }
    if (asset.backends.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "shader library has no backends");
    }

    auto hash = ContentHash(asset);
    if (!hash) {
        return std::move(hash).TakeError();
    }
    const std::filesystem::path root =
        options.cache_root / "parcel" / options.package / "shader_library" / hash.Value();
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to create shader cache directory");
    }

    const std::filesystem::path blobs = root.parent_path() / std::string(kBlobDirectoryName);
    std::filesystem::create_directories(blobs, error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to create shader blob directory");
    }

    ShaderMaterialization materialized;
    materialized.root = root;
    materialized.content_hash = hash.Value();

    std::string manifest =
        "{\n  \"version\": 1,\n  \"generated_by\": \"parcel_hbg\",\n  \"passes\": {\n";
    for (std::size_t backend_index = 0; backend_index < asset.backends.size(); ++backend_index) {
        const ShaderBackendLibrary& backend = asset.backends[backend_index];
        const std::string backend_name = BackendName(backend.backend);
        if (backend_name.empty()) {
            return MakeError(ErrorCode::Unsupported, "shader backend is not supported");
        }
        if (backend_index != 0) {
            manifest += ",\n";
        }
        manifest += "    \"";
        manifest += backend_name;
        manifest += "\": {\n";

        std::map<std::string, PassFiles> passes;
        for (const ShaderPassBinary& pass : backend.passes) {
            if (!ValidPassName(pass.pass)) {
                return MakeError(ErrorCode::InvalidArgument, "shader pass name is invalid");
            }
            if (pass.data.empty()) {
                return MakeError(ErrorCode::InvalidArgument, "shader pass binary is empty");
            }

            PassFiles& files = passes[pass.pass];
            const std::string suffix = StageSuffix(pass.stage);
            if (suffix.empty()) {
                return MakeError(ErrorCode::Unsupported, "shader stage is not supported");
            }
            std::string relative =
                "build/" + backend_name + "/" + pass.pass + "." + suffix + ".bin";
            std::size_t duplicate = 0;
            std::filesystem::path output = JoinPath(root, relative);
            auto exists = [&]() {
                return std::find(materialized.files.begin(), materialized.files.end(), output) !=
                       materialized.files.end();
            };
            while (exists()) {
                relative = "build/" + backend_name + "/" + pass.pass + "." + suffix + "." +
                           std::to_string(++duplicate) + ".bin";
                output = JoinPath(root, relative);
            }
            std::filesystem::create_directories(output.parent_path(), error);
            if (error) {
                return MakeError(ErrorCode::Io, "failed to create shader binary directory");
            }
            auto blob = EnsureBlob(blobs, pass.data);
            if (!blob) {
                return std::move(blob).TakeError();
            }
            auto linked = MaterializeBlob(blob.Value(), output, options.use_hard_links);
            if (!linked) {
                return std::move(linked).TakeError();
            }
            materialized.files.push_back(output);

            switch (pass.stage) {
            case ShaderStage::Vertex:
                if (!files.vertex.empty()) {
                    return MakeError(ErrorCode::Corrupt, "duplicate vertex shader pass");
                }
                files.vertex = relative;
                break;
            case ShaderStage::Fragment:
                files.fragments.push_back(relative);
                break;
            case ShaderStage::BufferFragment:
                files.buffer_fragments.push_back(relative);
                break;
            case ShaderStage::Compute:
                if (files.compute.empty()) {
                    files.compute = relative;
                } else {
                    files.computes.push_back(relative);
                }
                break;
            case ShaderStage::BufferCompute:
                if (files.buffer_compute.empty()) {
                    files.buffer_compute = relative;
                } else {
                    files.buffer_computes.push_back(relative);
                }
                break;
            }
        }

        if (passes.empty()) {
            return MakeError(ErrorCode::InvalidArgument, "shader backend has no passes");
        }
        bool first_pass = true;
        for (const auto& [name, files] : passes) {
            if (!first_pass) {
                manifest += ",\n";
            }
            first_pass = false;
            if (!files.fragments.empty() && files.vertex.empty()) {
                return MakeError(ErrorCode::Corrupt, "fragment shader pass has no vertex shader");
            }
            manifest += "      \"";
            manifest += JsonEscape(name);
            manifest += "\": {";
            bool has_field = false;
            if (!files.vertex.empty()) {
                manifest += "\"vertex\":\"";
                manifest += JsonEscape(files.vertex);
                manifest += "\"";
                has_field = true;
            }
            auto append_field = [&](std::string_view key, const std::vector<std::string>& values) {
                if (values.empty()) {
                    return;
                }
                if (has_field) {
                    manifest += ",";
                }
                manifest += "\"";
                manifest += key;
                manifest += "\":";
                AppendArray(manifest, values);
                has_field = true;
            };
            auto append_string_field = [&](std::string_view key, std::string_view value) {
                if (has_field) {
                    manifest += ",";
                }
                manifest += "\"";
                manifest += key;
                manifest += "\":\"";
                manifest += JsonEscape(value);
                manifest += "\"";
                has_field = true;
            };
            append_field("fragments", files.fragments);
            if (files.fragments.size() == 1) {
                append_string_field("fragment", files.fragments.front());
            }
            append_field("bufferFragments", files.buffer_fragments);
            if (files.buffer_fragments.size() == 1) {
                append_string_field("bufferFragment", files.buffer_fragments.front());
            }
            if (!files.compute.empty()) {
                if (has_field) {
                    manifest += ",";
                }
                manifest += "\"compute\":\"";
                manifest += JsonEscape(files.compute);
                manifest += "\"";
                has_field = true;
            }
            if (!files.buffer_compute.empty()) {
                if (has_field) {
                    manifest += ",";
                }
                manifest += "\"bufferCompute\":\"";
                manifest += JsonEscape(files.buffer_compute);
                manifest += "\"";
                has_field = true;
            }
            append_field("computes", files.computes);
            append_field("bufferComputes", files.buffer_computes);
            manifest += "}";
        }
        manifest += "\n    }";
    }
    manifest += "\n  }\n}\n";

    const std::filesystem::path manifest_path = root / "manifest.json";
    auto written_manifest = WriteText(manifest_path, manifest);
    if (!written_manifest) {
        return std::move(written_manifest).TakeError();
    }
    materialized.files.push_back(manifest_path);
    return materialized;
}

Result<std::size_t>
ShaderLibraryBootstrap::CleanupOldVersions(const ShaderMaterializeOptions& options,
                                           std::string_view keep_hash) {
    if (options.cache_root.empty() || options.package.empty() ||
        !parcel::core::path::IsSafeRelative(options.package) ||
        (keep_hash.empty() == false && !parcel::core::path::IsSafeRelative(keep_hash))) {
        return MakeError(ErrorCode::InvalidArgument, "shader cleanup options are invalid");
    }
    const std::filesystem::path root =
        options.cache_root / "parcel" / options.package / "shader_library";
    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        return std::size_t{0};
    }
    if (error) {
        return MakeError(ErrorCode::Io, "failed to inspect shader cache directory");
    }

    std::size_t removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error) {
            return MakeError(ErrorCode::Io, "failed to enumerate shader cache");
        }
        if (!entry.is_directory()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name == kBlobDirectoryName) {
            continue;
        }
        if (name == keep_hash) {
            continue;
        }
        std::filesystem::remove_all(entry.path(), error);
        if (error) {
            return MakeError(ErrorCode::Io, "failed to remove old shader cache");
        }
        ++removed;
    }

    const std::filesystem::path blobs = root / std::string(kBlobDirectoryName);
    std::error_code blob_error;
    if (std::filesystem::exists(blobs, blob_error)) {
        if (blob_error) {
            return MakeError(ErrorCode::Io, "failed to inspect shader blob cache");
        }
        CleanupOrphanBlobs(blobs);
    } else if (blob_error) {
        return MakeError(ErrorCode::Io, "failed to inspect shader blob cache");
    }
    return removed;
}

} // namespace parcel_hbg
