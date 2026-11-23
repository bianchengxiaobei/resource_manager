#include "texture_canonical.hpp"

#include "parcel/core/error.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/path.hpp"
#include "parcel/import/meta.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace parcel::import::detail {
namespace {

using core::ErrorCode;
using core::MakeError;

struct Candidate {
    std::string relative_path;
    std::string dedup_key;
    core::AssetGuid asset_guid;
    bool has_meta = false;
};

std::filesystem::path Utf8Path(std::string_view path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

std::string LowerExtension(std::string_view path) {
    std::string extension(core::path::Extension(path));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

core::Result<std::string> ProjectRelative(std::string_view root, std::string_view path) {
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(Utf8Path(path), Utf8Path(root), error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to resolve texture path: " + error.message())
            .WithPath(std::string(path));
    }
    const std::u8string value = relative.u8string();
    return core::path::Normalize(
        std::string(reinterpret_cast<const char*>(value.data()), value.size()));
}

} // namespace

core::Result<TextureCanonicalIndex> BuildTextureCanonicalIndex(std::string_view project_root,
                                                               std::string_view target,
                                                               std::span<const std::string> sources,
                                                               const ImporterRegistry& importers) {
    std::unordered_map<std::string, std::vector<Candidate>> groups;
    for (const std::string& source : sources) {
        const IAssetImporter* importer = importers.FindForExtension(LowerExtension(source));
        if (importer == nullptr || importer->Name() != "texture") {
            continue;
        }
        auto relative = ProjectRelative(project_root, source);
        if (!relative) {
            return std::move(relative).TakeError();
        }
        auto hash = core::Sha256File(source);
        if (!hash) {
            return std::move(hash).TakeError().WithPath(source);
        }

        std::error_code error;
        Candidate candidate;
        candidate.relative_path = std::move(relative).Value();
        candidate.has_meta =
            std::filesystem::is_regular_file(Utf8Path(AssetMetaPath(source)), error);
        std::string settings_hash;
        if (candidate.has_meta) {
            auto meta = LoadAssetMetaFile(AssetMetaPath(source));
            if (!meta) {
                return std::move(meta).TakeError().WithPath(source);
            }
            settings_hash = HashSettings(meta.Value().settings).ToHex();
        } else {
            settings_hash = HashSettings(MakeDefaultImportSettings("texture", target)).ToHex();
        }
        candidate.dedup_key = TextureDedupKey(hash.Value().ToHex(), settings_hash);
        candidate.asset_guid = TextureAssetGuidForContent(hash.Value().ToHex(), settings_hash);
        groups[candidate.dedup_key].push_back(std::move(candidate));
    }

    TextureCanonicalIndex index;
    for (auto& [dedup_key, candidates] : groups) {
        const Candidate* canonical = nullptr;
        for (const Candidate& candidate : candidates) {
            if (candidate.has_meta &&
                (canonical == nullptr || candidate.relative_path < canonical->relative_path)) {
                canonical = &candidate;
            }
        }
        if (canonical == nullptr) {
            for (const Candidate& candidate : candidates) {
                if (canonical == nullptr || candidate.relative_path < canonical->relative_path) {
                    canonical = &candidate;
                }
            }
        }
        if (canonical == nullptr) {
            continue;
        }
        const TextureCanonicalAsset entry{canonical->relative_path, canonical->asset_guid};
        index.by_content.emplace(dedup_key, entry);
        for (const Candidate& candidate : candidates) {
            index.by_path.emplace(candidate.relative_path, entry);
        }
    }
    return index;
}

} // namespace parcel::import::detail
