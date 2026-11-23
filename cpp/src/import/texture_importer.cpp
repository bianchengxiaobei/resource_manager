#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/error.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/path.hpp"
#include "parcel/core/platform_file.hpp"
#include "parcel/import/importer.hpp"
#include "parcel/import/meta.hpp"
#include "parcel/import/texture_dedup.hpp"
#include "texture_decode.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace parcel::import {
namespace {

using core::ErrorCode;
using core::MakeError;

class TextureImporter final : public IAssetImporter {
  public:
    std::string_view Name() const noexcept override { return "texture"; }
    std::uint32_t Version() const noexcept override { return 3; }

    bool Supports(std::string_view extension) const noexcept override {
        return extension == ".png" || extension == ".tga" || extension == ".jpg" ||
               extension == ".jpeg" || extension == ".hdr";
    }

    core::Result<ImportResult> Import(const ImportRequest& request) const override {
        const assets::JsonValue empty_settings;
        const assets::JsonValue& settings =
            request.settings != nullptr ? *request.settings : empty_settings;

        core::PlatformFile platform;
        auto file = platform.OpenRead(request.source_path);
        if (!file) {
            return std::move(file).TakeError();
        }
        auto bytes = core::ReadAll(*file.Value());
        if (!bytes) {
            return std::move(bytes).TakeError();
        }
        if (bytes.Value().empty()) {
            return MakeError(ErrorCode::InvalidArgument, "texture source is empty")
                .WithPath(request.source_path);
        }

        const core::Hash256 content_hash = core::Sha256(std::span<const std::byte>(bytes.Value()));
        const core::Hash256 settings_hash = HashSettings(settings);
        const TextureCanonicalAsset* canonical = FindCanonicalTexture(
            request.texture_index, content_hash.ToHex(), settings_hash.ToHex());

        auto decoded = DecodeTextureAsset(std::span<const std::byte>(bytes.Value()), settings,
                                          request.source_path);
        if (!decoded) {
            return std::move(decoded).TakeError();
        }
        ImportResult result;
        CookedAsset cooked;
        if (canonical != nullptr) {
            cooked.guid = canonical->asset_guid;
        }
        cooked.local_id = "texture";
        cooked.role = "texture";
        cooked.object = std::move(decoded).Value();
        result.assets.push_back(std::move(cooked));
        return result;
    }
};

} // namespace

std::shared_ptr<const IAssetImporter> MakeTextureImporter() {
    return std::make_shared<TextureImporter>();
}

} // namespace parcel::import
