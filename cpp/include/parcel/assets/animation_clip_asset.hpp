#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/math.hpp"
#include "parcel/scene/components.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace parcel::assets {

inline constexpr std::uint32_t kMaxAnimationChannels = 65536;
inline constexpr std::uint32_t kMaxAnimationKeys = 1000000;

enum class AnimationPath : std::uint8_t { Translation, Rotation, Scale, Weights };
enum class AnimationInterpolation : std::uint8_t { Step, Linear, CubicSpline };

struct AnimationChannel {
    scene::SceneEntityId target;
    AnimationPath path = AnimationPath::Translation;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
    std::uint32_t target_index = 0;
    std::vector<float> times;
    std::vector<Vec4> values;
    std::vector<Vec4> in_tangents;
    std::vector<Vec4> out_tangents;
};

class AnimationClipAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kAnimationClipAssetType; }

    std::string name;
    float duration = 0.0f;
    std::vector<AnimationChannel> channels;
};

[[nodiscard]] Result<void> ValidateAnimationClip(const AnimationClipAsset& clip);

} // namespace parcel::assets
