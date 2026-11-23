#include "parcel/assets/ies_profile_asset.hpp"

#include "parcel/core/error.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace parcel::assets {
namespace {

using core::ErrorCode;
using core::MakeError;

Result<void> ValidateAngles(const std::vector<float>& angles, std::uint32_t minimum,
                            std::uint32_t maximum, float upper, const char* what) {
    if (angles.size() < minimum || angles.size() > maximum) {
        return MakeError(ErrorCode::InvalidArgument, std::string(what) + " count is out of range");
    }
    float previous = 0.0f;
    bool first = true;
    for (float angle : angles) {
        if (!std::isfinite(angle) || angle < 0.0f || angle > upper) {
            return MakeError(ErrorCode::InvalidArgument,
                             std::string(what) + " angle is out of range");
        }
        if (!first && angle < previous) {
            return MakeError(ErrorCode::InvalidArgument,
                             std::string(what) + " angles must be sorted");
        }
        previous = angle;
        first = false;
    }
    return {};
}

} // namespace

Result<void> ValidateIesProfile(const IesProfileAsset& profile) {
    auto vertical =
        ValidateAngles(profile.vertical_angles, 2, kMaxIesAngles, 180.0f, "IES vertical");
    if (!vertical) {
        return std::move(vertical).TakeError();
    }
    auto horizontal =
        ValidateAngles(profile.horizontal_angles, 1, kMaxIesAngles, 360.0f, "IES horizontal");
    if (!horizontal) {
        return std::move(horizontal).TakeError();
    }

    const std::uint64_t expected = static_cast<std::uint64_t>(profile.vertical_angles.size()) *
                                   static_cast<std::uint64_t>(profile.horizontal_angles.size());
    if (profile.values.size() != expected) {
        return MakeError(ErrorCode::InvalidArgument, "IES value count does not match angle table");
    }

    float maximum = 0.0f;
    for (float value : profile.values) {
        if (!std::isfinite(value) || value < 0.0f) {
            return MakeError(ErrorCode::InvalidArgument, "IES candela value is invalid");
        }
        maximum = std::max(maximum, value);
    }
    if (maximum <= 0.0f) {
        return MakeError(ErrorCode::InvalidArgument, "IES profile has no positive candela values");
    }
    return {};
}

} // namespace parcel::assets
