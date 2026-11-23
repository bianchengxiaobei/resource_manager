#include "parcel/assets/raw_file_asset.hpp"
#include "parcel/core/error.hpp"
#include "parcel/core/path.hpp"
#include "parcel/core/platform_file.hpp"
#include "parcel/import/importer.hpp"

#include <memory>
#include <string>
#include <vector>

namespace parcel::import {
namespace {

using core::ErrorCode;
using core::MakeError;

core::Result<std::vector<std::byte>> ReadSource(std::string_view path) {
    core::PlatformFile platform;
    auto file = platform.OpenRead(path);
    if (!file) {
        return std::move(file).TakeError();
    }
    return core::ReadAll(*file.Value());
}

class TextImporter final : public IAssetImporter {
  public:
    std::string_view Name() const noexcept override { return "text"; }
    std::uint32_t Version() const noexcept override { return 1; }

    bool Supports(std::string_view extension) const noexcept override {
        return extension == ".txt" || extension == ".json" || extension == ".xml" ||
               extension == ".yaml" || extension == ".yml" || extension == ".ini" ||
               extension == ".csv" || extension == ".md" || extension == ".hlsl" ||
               extension == ".glsl" || extension == ".vert" || extension == ".frag" ||
               extension == ".comp";
    }

    core::Result<ImportResult> Import(const ImportRequest& request) const override {
        auto bytes = ReadSource(request.source_path);
        if (!bytes) {
            return std::move(bytes).TakeError();
        }
        auto asset = std::make_shared<assets::TextAsset>();
        asset->name = std::string(core::path::FileName(request.source_path));
        asset->text.assign(reinterpret_cast<const char*>(bytes.Value().data()),
                           bytes.Value().size());

        ImportResult result;
        CookedAsset cooked;
        cooked.local_id = "text";
        cooked.role = "text";
        cooked.object = std::move(asset);
        result.assets.push_back(std::move(cooked));
        return result;
    }
};

class RawFileImporter final : public IAssetImporter {
  public:
    std::string_view Name() const noexcept override { return "raw"; }
    std::uint32_t Version() const noexcept override { return 1; }

    bool Supports(std::string_view extension) const noexcept override {
        return extension == ".bin" || extension == ".dat" || extension == ".bytes" ||
               extension == ".raw";
    }

    core::Result<ImportResult> Import(const ImportRequest& request) const override {
        auto bytes = ReadSource(request.source_path);
        if (!bytes) {
            return std::move(bytes).TakeError();
        }
        auto asset = std::make_shared<assets::RawFileAsset>();
        asset->name = std::string(core::path::FileName(request.source_path));
        asset->data = std::move(bytes).Value();

        ImportResult result;
        CookedAsset cooked;
        cooked.local_id = "raw";
        cooked.role = "raw";
        cooked.object = std::move(asset);
        result.assets.push_back(std::move(cooked));
        return result;
    }
};

} // namespace

std::shared_ptr<const IAssetImporter> MakeTextImporter() {
    return std::make_shared<TextImporter>();
}

std::shared_ptr<const IAssetImporter> MakeRawFileImporter() {
    return std::make_shared<RawFileImporter>();
}

} // namespace parcel::import
