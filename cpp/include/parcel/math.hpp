#pragma once

#include <cstddef>

namespace parcel {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    friend bool operator==(const Vec2&, const Vec2&) = default;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    friend bool operator==(const Vec3&, const Vec3&) = default;
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    friend bool operator==(const Vec4&, const Vec4&) = default;
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    static constexpr Quat Identity() noexcept { return {}; }

    friend bool operator==(const Quat&, const Quat&) = default;
};

struct Bounds {
    Vec3 center{};
    Vec3 extents{};

    friend bool operator==(const Bounds&, const Bounds&) = default;
};

struct Mat4 {
    float m[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    friend bool operator==(const Mat4&, const Mat4&) = default;
};

} // namespace parcel
