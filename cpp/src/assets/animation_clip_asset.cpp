#include "parcel/assets/animation_clip_asset.hpp"

#include "parcel/core/error.hpp"

#include <cmath>

namespace parcel::assets {
namespace {

using core::ErrorCode;
using core::MakeError;

bool IsFinite(const Vec4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::isfinite(value.w);
}

} // namespace

Result<void> ValidateAnimationClip(const AnimationClipAsset& clip) {
    if (!std::isfinite(clip.duration) || clip.duration < 0.0f) {
        return MakeError(ErrorCode::InvalidArgument, "animation clip duration is invalid");
    }
    if (clip.channels.empty() || clip.channels.size() > kMaxAnimationChannels) {
        return MakeError(ErrorCode::InvalidArgument, "animation clip channel count is invalid");
    }

    for (const AnimationChannel& channel : clip.channels) {
        if (channel.target.IsRoot()) {
            return MakeError(ErrorCode::InvalidArgument, "animation channel target is not set");
        }
        if (static_cast<std::uint8_t>(channel.path) >
            static_cast<std::uint8_t>(AnimationPath::Weights)) {
            return MakeError(ErrorCode::InvalidArgument, "animation channel path is invalid");
        }
        if (static_cast<std::uint8_t>(channel.interpolation) >
            static_cast<std::uint8_t>(AnimationInterpolation::CubicSpline)) {
            return MakeError(ErrorCode::InvalidArgument,
                             "animation channel interpolation is invalid");
        }
        if (channel.times.empty() || channel.times.size() > kMaxAnimationKeys ||
            channel.times.size() != channel.values.size()) {
            return MakeError(ErrorCode::InvalidArgument, "animation channel key count is invalid");
        }
        if ((!channel.in_tangents.empty() && channel.in_tangents.size() != channel.times.size()) ||
            (!channel.out_tangents.empty() &&
             channel.out_tangents.size() != channel.times.size()) ||
            channel.in_tangents.empty() != channel.out_tangents.empty()) {
            return MakeError(ErrorCode::InvalidArgument,
                             "animation channel tangent count is invalid");
        }

        float previous = -1.0f;
        for (std::size_t index = 0; index < channel.times.size(); ++index) {
            const float time = channel.times[index];
            if (!std::isfinite(time) || time < 0.0f || time < previous ||
                time > clip.duration + 1.0e-4f) {
                return MakeError(ErrorCode::InvalidArgument,
                                 "animation channel time is out of range");
            }
            if (!IsFinite(channel.values[index])) {
                return MakeError(ErrorCode::InvalidArgument,
                                 "animation channel value is not finite");
            }
            if (!channel.in_tangents.empty() || !channel.out_tangents.empty()) {
                if (!IsFinite(channel.in_tangents[index]) ||
                    !IsFinite(channel.out_tangents[index])) {
                    return MakeError(ErrorCode::InvalidArgument,
                                     "animation channel tangent is not finite");
                }
            }
            previous = time;
        }
    }
    return {};
}

} // namespace parcel::assets
