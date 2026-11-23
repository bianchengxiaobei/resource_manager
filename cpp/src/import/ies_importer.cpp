#include "parcel/import/ies_profile.hpp"

#include "parcel/assets/ies_profile_asset.hpp"
#include "parcel/core/error.hpp"
#include "parcel/core/path.hpp"
#include "parcel/core/platform_file.hpp"
#include "parcel/import/importer.hpp"
#include "parcel/import/meta.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace parcel::import {
namespace {

using core::ErrorCode;
using core::MakeError;

constexpr std::uint64_t kMaxIesFileBytes = 32U * 1000U * 1000U;

std::string LowerAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

core::Result<assets::IesProfileAsset> ParseIesProfile(std::string_view text) {
    const std::string lowered = LowerAscii(text);
    const std::size_t tilt = lowered.find("tilt=");
    if (tilt == std::string::npos) {
        return MakeError(ErrorCode::InvalidArgument, "IES profile is missing TILT");
    }
    const std::size_t value_start = tilt + 5;
    std::size_t value_end = value_start;
    while (value_end < text.size() &&
           std::isspace(static_cast<unsigned char>(text[value_end])) == 0) {
        ++value_end;
    }
    if (LowerAscii(text.substr(value_start, value_end - value_start)) != "none") {
        return MakeError(ErrorCode::InvalidArgument, "IES profile must use TILT=NONE");
    }
    const std::size_t line_end = text.find('\n', value_end);
    if (line_end == std::string_view::npos) {
        return MakeError(ErrorCode::InvalidArgument, "IES profile is missing its header");
    }

    std::istringstream stream(std::string(text.substr(line_end + 1)));
    stream.imbue(std::locale::classic());
    long long lamp_count = 0;
    double lumens = 0.0;
    double multiplier = 0.0;
    long long vertical_count = 0;
    long long horizontal_count = 0;
    long long photometric_type = 0;
    long long units_type = 0;
    double width = 0.0;
    double length = 0.0;
    double height = 0.0;
    if (!(stream >> lamp_count >> lumens >> multiplier >> vertical_count >> horizontal_count >>
          photometric_type >> units_type >> width >> length >> height)) {
        return MakeError(ErrorCode::InvalidArgument, "invalid IES header");
    }
    if (vertical_count < 2 || vertical_count > assets::kMaxIesAngles || horizontal_count < 1 ||
        horizontal_count > assets::kMaxIesAngles) {
        return MakeError(ErrorCode::InvalidArgument, "unsupported IES angle count");
    }
    if (lamp_count < 0) {
        return MakeError(ErrorCode::InvalidArgument, "invalid IES lamp count");
    }

    assets::IesProfileAsset profile;
    const auto read_angle = [&](std::vector<float>& values, std::size_t count,
                                std::string_view what) -> core::Result<void> {
        values.resize(count);
        for (float& value : values) {
            double parsed = 0.0;
            if (!(stream >> parsed) || !std::isfinite(parsed) ||
                std::abs(parsed) > std::numeric_limits<float>::max()) {
                return MakeError(ErrorCode::InvalidArgument,
                                 std::string("truncated IES ") + std::string(what));
            }
            value = static_cast<float>(parsed);
        }
        return {};
    };
    PARCEL_TRY(read_angle(profile.vertical_angles, static_cast<std::size_t>(vertical_count),
                          "vertical angles"));
    PARCEL_TRY(read_angle(profile.horizontal_angles, static_cast<std::size_t>(horizontal_count),
                          "horizontal angles"));

    const std::size_t value_count =
        static_cast<std::size_t>(vertical_count) * static_cast<std::size_t>(horizontal_count);
    profile.values.resize(value_count);
    for (float& value : profile.values) {
        double parsed = 0.0;
        if (!(stream >> parsed) || !std::isfinite(parsed) || parsed < 0.0 ||
            parsed > std::numeric_limits<float>::max()) {
            return MakeError(ErrorCode::InvalidArgument, "truncated IES candela values");
        }
        value = static_cast<float>(parsed);
    }
    PARCEL_TRY(assets::ValidateIesProfile(profile));
    return profile;
}

class IesProfileImporter final : public IAssetImporter {
  public:
    std::string_view Name() const noexcept override { return "ies"; }
    std::uint32_t Version() const noexcept override { return 1; }

    bool Supports(std::string_view extension) const noexcept override {
        return extension == ".ies";
    }

    core::Result<ImportResult> Import(const ImportRequest& request) const override {
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
            return MakeError(ErrorCode::InvalidArgument, "IES source is empty")
                .WithPath(request.source_path);
        }
        if (bytes.Value().size() > kMaxIesFileBytes) {
            return MakeError(ErrorCode::InvalidArgument, "IES source is too large")
                .WithPath(request.source_path);
        }

        std::string_view text(reinterpret_cast<const char*>(bytes.Value().data()),
                              bytes.Value().size());
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
            static_cast<unsigned char>(text[1]) == 0xbb &&
            static_cast<unsigned char>(text[2]) == 0xbf) {
            text.remove_prefix(3);
        }
        auto profile = ParseIesProfile(text);
        if (!profile) {
            return std::move(profile).TakeError().WithPath(request.source_path);
        }
        profile.Value().name = std::string(core::path::FileName(request.source_path));

        ImportResult result;
        CookedAsset cooked;
        cooked.local_id = "ies";
        cooked.role = "ies";
        cooked.object = std::make_shared<assets::IesProfileAsset>(std::move(profile).Value());
        result.assets.push_back(std::move(cooked));
        return result;
    }
};

} // namespace

core::AssetGuid DefaultIesAssetGuid(std::string_view project_relative_path) {
    return MakeSubAssetGuid(MakeSourceGuid(project_relative_path), "ies", "ies");
}

std::shared_ptr<const IAssetImporter> MakeIesProfileImporter() {
    return std::make_shared<IesProfileImporter>();
}

} // namespace parcel::import
