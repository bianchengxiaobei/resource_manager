#pragma once

#include "parcel/math.hpp"

#include <cmath>

namespace parcel::import {

struct TextureUvTransform {
    Vec2 offset{};
    float rotation = 0.0f;
    Vec2 scale{1.0f, 1.0f};

    [[nodiscard]] bool IsIdentity() const noexcept {
        return offset.x == 0.0f && offset.y == 0.0f && rotation == 0.0f && scale.x == 1.0f &&
               scale.y == 1.0f;
    }

    [[nodiscard]] Vec2 Apply(const Vec2& uv) const noexcept {
        const float cosine = std::cos(rotation);
        const float sine = std::sin(rotation);
        const float scaled_u = uv.x * scale.x;
        const float scaled_v = uv.y * scale.y;
        return {cosine * scaled_u - sine * scaled_v + offset.x,
                sine * scaled_u + cosine * scaled_v + offset.y};
    }
};

inline bool NearlyEqual(float left, float right) noexcept {
    return std::abs(left - right) <= 1.0e-6f;
}

inline bool TransformsMatch(const TextureUvTransform& left,
                            const TextureUvTransform& right) noexcept {
    return NearlyEqual(left.offset.x, right.offset.x) &&
           NearlyEqual(left.offset.y, right.offset.y) &&
           NearlyEqual(left.rotation, right.rotation) && NearlyEqual(left.scale.x, right.scale.x) &&
           NearlyEqual(left.scale.y, right.scale.y);
}

} // namespace parcel::import
