#include "parcel/hbg/scene_render_bridge.hpp"

#include "parcel/assets/animation_clip_asset.hpp"
#include "parcel/assets/ies_profile_asset.hpp"
#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/mesh_asset.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/error.hpp"
#include "parcel/hbg/asset_materializer.hpp"
#include "parcel/scene/components.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <tuple>
#include <unordered_set>
#include <utility>

#include <render/lighting/gpu_light_data.h>

namespace parcel_hbg {
namespace {

using parcel::core::ErrorCode;
using parcel::core::MakeError;
using parcel::scene::AnimationPlayerComponent;
using parcel::scene::CameraComponent;
using parcel::scene::DecalComponent;
using parcel::scene::LightComponent;
using parcel::scene::LocalFogVolumeComponent;
using parcel::scene::MeshRendererComponent;
using parcel::scene::MorphComponent;
using parcel::scene::ReflectionProbeComponent;
using parcel::scene::SceneEntityId;
using parcel::scene::SkinComponent;
using parcel::scene::TransformComponent;

hbg::Vec2 ToHbg(const parcel::Vec2& value) { return hbg::Vec2{value.x, value.y}; }

hbg::Vec3 ToHbg(const parcel::Vec3& value) { return hbg::Vec3{value.x, value.y, value.z}; }

hbg::Vec4 ToHbg(const parcel::Vec4& value) { return hbg::Vec4{value.x, value.y, value.z, value.w}; }

hbg::Mat4 ToHbg(const parcel::Mat4& value) {
    hbg::Mat4 result;
    for (std::size_t index = 0; index < 16; ++index) {
        result.values[index] = value.m[index];
    }
    return result;
}

hbg::Mat4 ToMatrix(const TransformComponent& transform) {
    const float x = transform.rotation.x;
    const float y = transform.rotation.y;
    const float z = transform.rotation.z;
    const float w = transform.rotation.w;
    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float xz = x * z;
    const float yz = y * z;
    const float wx = w * x;
    const float wy = w * y;
    const float wz = w * z;

    hbg::Mat4 rotation = hbg::Mat4::identity();
    rotation[0] = 1.0f - 2.0f * (yy + zz);
    rotation[1] = 2.0f * (xy + wz);
    rotation[2] = 2.0f * (xz - wy);
    rotation[4] = 2.0f * (xy - wz);
    rotation[5] = 1.0f - 2.0f * (xx + zz);
    rotation[6] = 2.0f * (yz + wx);
    rotation[8] = 2.0f * (xz + wy);
    rotation[9] = 2.0f * (yz - wx);
    rotation[10] = 1.0f - 2.0f * (xx + yy);

    hbg::Mat4 scale = hbg::Mat4::identity();
    scale[0] = transform.scale.x;
    scale[5] = transform.scale.y;
    scale[10] = transform.scale.z;

    hbg::Mat4 translation = hbg::Mat4::identity();
    translation[12] = transform.position.x;
    translation[13] = transform.position.y;
    translation[14] = transform.position.z;
    return translation * rotation * scale;
}

Result<hbg::Mat4> ComputeWorld(const ::entt::registry& registry, ::entt::entity entity,
                               const std::unordered_map<std::uint64_t, ::entt::entity>& entities,
                               std::size_t depth) {
    if (depth > 1024) {
        return MakeError(ErrorCode::Corrupt, "scene entity parent depth exceeds limit");
    }
    if (!registry.valid(entity)) {
        return MakeError(ErrorCode::Corrupt, "scene entity is invalid");
    }

    hbg::Mat4 parent_world = hbg::Mat4::identity();
    const TransformComponent* transform = registry.try_get<TransformComponent>(entity);
    const parcel_entt::ParentEntityComponent* parent =
        registry.try_get<parcel_entt::ParentEntityComponent>(entity);
    if (parent != nullptr && parent->entity != ::entt::null) {
        auto resolved = ComputeWorld(registry, parent->entity, entities, depth + 1);
        if (!resolved) {
            return std::move(resolved).TakeError();
        }
        parent_world = resolved.Value();
    } else if (transform != nullptr && !transform->parent.IsRoot()) {
        const auto found = entities.find(transform->parent.value);
        if (found == entities.end()) {
            return MakeError(ErrorCode::Corrupt, "scene entity parent was not instantiated");
        }
        auto resolved = ComputeWorld(registry, found->second, entities, depth + 1);
        if (!resolved) {
            return std::move(resolved).TakeError();
        }
        parent_world = resolved.Value();
    }

    if (transform == nullptr) {
        return parent_world;
    }
    return parent_world * ToMatrix(*transform);
}

std::string EntityName(const ::entt::registry& registry, ::entt::entity entity) {
    if (const parcel_entt::SceneEntityComponent* tag =
            registry.try_get<parcel_entt::SceneEntityComponent>(entity)) {
        return tag->name;
    }
    return "entity";
}

std::uint64_t EntityKey(::entt::entity entity) noexcept {
    return static_cast<std::uint64_t>(entity);
}

std::uint32_t EntitySceneOrder(const ::entt::registry& registry, ::entt::entity entity) noexcept {
    const auto* tag = registry.try_get<parcel_entt::SceneEntityComponent>(entity);
    return tag == nullptr ? std::numeric_limits<std::uint32_t>::max() : tag->scene_order;
}

std::uint64_t EntitySceneId(const ::entt::registry& registry, ::entt::entity entity) noexcept {
    const auto* tag = registry.try_get<parcel_entt::SceneEntityComponent>(entity);
    return tag == nullptr ? std::numeric_limits<std::uint64_t>::max() : tag->id.value;
}

bool EntityOrderLess(const ::entt::registry& registry, ::entt::entity lhs, ::entt::entity rhs) {
    const std::uint32_t lhs_order = EntitySceneOrder(registry, lhs);
    const std::uint32_t rhs_order = EntitySceneOrder(registry, rhs);
    if (lhs_order != rhs_order) {
        return lhs_order < rhs_order;
    }
    const std::uint64_t lhs_id = EntitySceneId(registry, lhs);
    const std::uint64_t rhs_id = EntitySceneId(registry, rhs);
    if (lhs_id != rhs_id) {
        return lhs_id < rhs_id;
    }
    return EntityKey(lhs) < EntityKey(rhs);
}

Result<std::unordered_map<std::uint64_t, int>>
BuildSceneNodes(const ::entt::registry& registry,
                const std::unordered_map<std::uint64_t, ::entt::entity>& entities,
                std::vector<hbg::asset::SceneNode>& nodes) {
    std::vector<::entt::entity> ordered;
    for (const auto entity : registry.view<const TransformComponent>()) {
        ordered.push_back(entity);
    }
    std::sort(ordered.begin(), ordered.end(), [&](::entt::entity lhs, ::entt::entity rhs) {
        return EntityOrderLess(registry, lhs, rhs);
    });

    nodes.clear();
    nodes.reserve(ordered.size());
    for (const auto entity : ordered) {
        const TransformComponent& transform = registry.get<const TransformComponent>(entity);
        hbg::asset::SceneNode node;
        node.name = EntityName(registry, entity);
        node.translation = ToHbg(transform.position);
        node.rotation = hbg::Vec4{transform.rotation.x, transform.rotation.y, transform.rotation.z,
                                  transform.rotation.w};
        node.scale = ToHbg(transform.scale);
        node.baseTranslation = node.translation;
        node.baseRotation = node.rotation;
        node.baseScale = node.scale;
        nodes.push_back(std::move(node));
    }

    std::unordered_map<std::uint64_t, int> node_indices;
    node_indices.reserve(nodes.size());
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        node_indices.emplace(EntityKey(ordered[index]), static_cast<int>(index));
    }

    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const TransformComponent& transform =
            registry.get<const TransformComponent>(ordered[index]);
        ::entt::entity parent_entity = ::entt::null;
        if (const auto* parent =
                registry.try_get<parcel_entt::ParentEntityComponent>(ordered[index])) {
            if (parent->entity != ::entt::null) {
                parent_entity = parent->entity;
            }
        }
        if (parent_entity == ::entt::null && !transform.parent.IsRoot()) {
            const auto found = entities.find(transform.parent.value);
            if (found != entities.end()) {
                parent_entity = found->second;
            }
        }
        if (parent_entity != ::entt::null) {
            const auto found = node_indices.find(EntityKey(parent_entity));
            if (found != node_indices.end()) {
                nodes[index].parent = found->second;
            }
        }
    }
    return node_indices;
}

hbg::asset::AnimationPath ToHbg(parcel::assets::AnimationPath path) {
    switch (path) {
    case parcel::assets::AnimationPath::Translation:
        return hbg::asset::AnimationPath::Translation;
    case parcel::assets::AnimationPath::Rotation:
        return hbg::asset::AnimationPath::Rotation;
    case parcel::assets::AnimationPath::Scale:
        return hbg::asset::AnimationPath::Scale;
    case parcel::assets::AnimationPath::Weights:
        return hbg::asset::AnimationPath::Weights;
    }
    return hbg::asset::AnimationPath::Translation;
}

hbg::asset::AnimationInterpolation ToHbg(parcel::assets::AnimationInterpolation interpolation) {
    switch (interpolation) {
    case parcel::assets::AnimationInterpolation::Step:
        return hbg::asset::AnimationInterpolation::Step;
    case parcel::assets::AnimationInterpolation::Linear:
        return hbg::asset::AnimationInterpolation::Linear;
    case parcel::assets::AnimationInterpolation::CubicSpline:
        return hbg::asset::AnimationInterpolation::CubicSpline;
    }
    return hbg::asset::AnimationInterpolation::Linear;
}

std::vector<hbg::Vec4> ToHbg(const std::vector<parcel::Vec4>& values) {
    std::vector<hbg::Vec4> result;
    result.reserve(values.size());
    for (const parcel::Vec4& value : values) {
        result.push_back(ToHbg(value));
    }
    return result;
}

Result<std::shared_ptr<parcel::assets::AssetObject>>
ResolveTypedAsset(const SceneRenderBridge::AssetResolver& resolver, AssetGuid guid,
                  parcel::assets::TypeId expected_type) {
    if (!resolver) {
        return MakeError(ErrorCode::InvalidArgument, "scene asset resolver is not set");
    }
    auto asset = resolver(guid);
    if (!asset) {
        return std::move(asset).TakeError();
    }
    if (asset.Value() == nullptr) {
        return MakeError(ErrorCode::NotFound, "scene asset resolver returned null");
    }
    if (asset.Value()->Type() != expected_type) {
        return MakeError(ErrorCode::Corrupt, "scene asset type does not match its reference");
    }
    return asset.Value();
}

Result<std::shared_ptr<parcel::assets::TextureAsset>>
ResolveTextureAsset(const SceneRenderBridge::AssetResolver& resolver, AssetGuid guid) {
    auto asset = ResolveTypedAsset(resolver, guid, parcel::assets::kTextureAssetType);
    if (!asset) {
        return std::move(asset).TakeError();
    }
    return std::static_pointer_cast<parcel::assets::TextureAsset>(asset.Value());
}

float HalfToFloat(std::uint16_t value) {
    const std::uint32_t sign = (value >> 15U) & 1U;
    const std::uint32_t exponent = (value >> 10U) & 0x1fU;
    std::uint32_t mantissa = value & 0x3ffU;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa != 0) {
            std::uint32_t normalized = 113U;
            while ((mantissa & 0x400U) == 0) {
                mantissa <<= 1U;
                --normalized;
            }
            mantissa &= 0x3ffU;
            bits = (sign << 31U) | (normalized << 23U) | (mantissa << 13U);
        } else {
            bits = sign << 31U;
        }
    } else if (exponent == 0x1fU) {
        bits = (sign << 31U) | (0xffU << 23U) | (mantissa << 13U);
    } else {
        bits = (sign << 31U) | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

Result<float> ReadLuminance(const parcel::assets::TextureAsset& texture, std::uint32_t x,
                            std::uint32_t y) {
    if (texture.mips.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "light cookie has no mip levels");
    }
    const parcel::assets::TextureMip& mip = texture.mips.front();
    if (mip.width == 0 || mip.height == 0 || x >= mip.width || y >= mip.height) {
        return MakeError(ErrorCode::Corrupt, "light cookie mip is invalid");
    }

    std::size_t bytes_per_pixel = 0;
    switch (texture.format) {
    case parcel::assets::TextureFormat::RGBA8:
    case parcel::assets::TextureFormat::BGRA8:
        bytes_per_pixel = 4;
        break;
    case parcel::assets::TextureFormat::RGBA16F:
        bytes_per_pixel = 8;
        break;
    case parcel::assets::TextureFormat::RGBA32F:
        bytes_per_pixel = 16;
        break;
    default:
        return MakeError(ErrorCode::Unsupported,
                         "compressed light cookie textures are not supported");
    }

    const std::size_t row_offset = static_cast<std::size_t>(y) * mip.row_pitch;
    const std::size_t pixel_offset = static_cast<std::size_t>(x) * bytes_per_pixel;
    if (mip.row_pitch < static_cast<std::uint32_t>(mip.width * bytes_per_pixel) ||
        row_offset > mip.data.size() || pixel_offset > mip.data.size() - row_offset ||
        bytes_per_pixel > mip.data.size() - row_offset - pixel_offset) {
        return MakeError(ErrorCode::Corrupt, "light cookie mip data is truncated");
    }
    const std::byte* pixel = mip.data.data() + row_offset + pixel_offset;

    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    switch (texture.format) {
    case parcel::assets::TextureFormat::RGBA8:
        red = static_cast<float>(std::to_integer<std::uint8_t>(pixel[0])) / 255.0f;
        green = static_cast<float>(std::to_integer<std::uint8_t>(pixel[1])) / 255.0f;
        blue = static_cast<float>(std::to_integer<std::uint8_t>(pixel[2])) / 255.0f;
        break;
    case parcel::assets::TextureFormat::BGRA8:
        blue = static_cast<float>(std::to_integer<std::uint8_t>(pixel[0])) / 255.0f;
        green = static_cast<float>(std::to_integer<std::uint8_t>(pixel[1])) / 255.0f;
        red = static_cast<float>(std::to_integer<std::uint8_t>(pixel[2])) / 255.0f;
        break;
    case parcel::assets::TextureFormat::RGBA16F: {
        std::uint16_t channels[4]{};
        std::memcpy(channels, pixel, sizeof(channels));
        red = HalfToFloat(channels[0]);
        green = HalfToFloat(channels[1]);
        blue = HalfToFloat(channels[2]);
        break;
    }
    case parcel::assets::TextureFormat::RGBA32F: {
        float channels[4]{};
        std::memcpy(channels, pixel, sizeof(channels));
        red = channels[0];
        green = channels[1];
        blue = channels[2];
        break;
    }
    default:
        break;
    }
    const float luminance = 0.2126f * red + 0.7152f * green + 0.0722f * blue;
    return std::isfinite(luminance) ? luminance : 0.0f;
}

Result<float> SampleLuminance(const parcel::assets::TextureAsset& texture, hbg::Vec2 uv) {
    if (texture.mips.empty() || texture.mips.front().width == 0 ||
        texture.mips.front().height == 0) {
        return MakeError(ErrorCode::InvalidArgument, "light cookie texture has no valid mip");
    }
    const std::uint32_t width = texture.mips.front().width;
    const std::uint32_t height = texture.mips.front().height;
    const float x = std::clamp(uv.x, 0.0f, 1.0f) * static_cast<float>(width - 1U);
    const float y = std::clamp(uv.y, 0.0f, 1.0f) * static_cast<float>(height - 1U);
    const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(x));
    const std::uint32_t y0 = static_cast<std::uint32_t>(std::floor(y));
    const std::uint32_t x1 = std::min(x0 + 1U, width - 1U);
    const std::uint32_t y1 = std::min(y0 + 1U, height - 1U);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    PARCEL_TRY_DECL(float, top_left, ReadLuminance(texture, x0, y0));
    PARCEL_TRY_DECL(float, top_right, ReadLuminance(texture, x1, y0));
    PARCEL_TRY_DECL(float, bottom_left, ReadLuminance(texture, x0, y1));
    PARCEL_TRY_DECL(float, bottom_right, ReadLuminance(texture, x1, y1));
    const float low = top_left * (1.0f - tx) + top_right * tx;
    const float high = bottom_left * (1.0f - tx) + bottom_right * tx;
    return low * (1.0f - ty) + high * ty;
}

hbg::Vec3 OctahedralDecodeDirection(hbg::Vec2 encoded) {
    hbg::Vec2 point(encoded.x * 2.0f - 1.0f, encoded.y * 2.0f - 1.0f);
    hbg::Vec3 normal(point.x, point.y, 1.0f - std::abs(point.x) - std::abs(point.y));
    if (normal.z < 0.0f) {
        normal.x = (1.0f - std::abs(normal.y)) * (normal.x >= 0.0f ? 1.0f : -1.0f);
        normal.y = (1.0f - std::abs(normal.x)) * (normal.y >= 0.0f ? 1.0f : -1.0f);
    }
    const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (length <= 1.0e-8f) {
        return {0.0f, 1.0f, 0.0f};
    }
    normal.x /= length;
    normal.y /= length;
    normal.z /= length;
    return normal;
}

hbg::Vec2 OctahedralDecode(hbg::Vec2 encoded) {
    const hbg::Vec3 normal = OctahedralDecodeDirection(encoded);
    constexpr float kPi = 3.14159265358979323846f;
    return {std::atan2(normal.x, normal.z) * (0.5f / kPi) + 0.5f,
            std::acos(std::clamp(normal.y, -1.0f, 1.0f)) / kPi};
}

std::uint32_t NextCookiePowerOfTwo(std::uint32_t value) {
    std::uint32_t result = 1;
    while (result < value && result < hbg::render::kCookieAtlasMaxNativeSize) {
        result <<= 1U;
    }
    return std::clamp(result, 8U, hbg::render::kCookieAtlasMaxNativeSize);
}

Result<void> LoadCookieSamples(const parcel::assets::TextureAsset& texture,
                               hbg::render::Light& light) {
    constexpr std::uint32_t kLegacySize = hbg::render::kCookieLegacySize;
    static_assert(kLegacySize * kLegacySize <= std::tuple_size_v<decltype(light.cookieSamples)>,
                  "cookie sample array is too small");
    for (std::uint32_t y = 0; y < kLegacySize; ++y) {
        for (std::uint32_t x = 0; x < kLegacySize; ++x) {
            const hbg::Vec2 uv((static_cast<float>(x) + 0.5f) / static_cast<float>(kLegacySize),
                               (static_cast<float>(y) + 0.5f) / static_cast<float>(kLegacySize));
            const hbg::Vec2 source_uv =
                light.cookieType == hbg::render::LightCookieType::PointOctahedral
                    ? OctahedralDecode(uv)
                    : uv;
            PARCEL_TRY_DECL(float, value, SampleLuminance(texture, source_uv));
            light.cookieSamples[static_cast<std::size_t>(y) * kLegacySize + x] =
                std::clamp(value, 0.0f, 1.0f);
        }
    }

    if (light.cookieType == hbg::render::LightCookieType::PointOctahedral) {
        const std::uint32_t native_size =
            NextCookiePowerOfTwo(std::max(texture.mips.front().width, texture.mips.front().height));
        light.cookieNativeWidth = native_size;
        light.cookieNativeHeight = native_size;
        light.cookieNative.assign(static_cast<std::size_t>(native_size) * native_size, 1.0f);
        for (std::uint32_t y = 0; y < native_size; ++y) {
            for (std::uint32_t x = 0; x < native_size; ++x) {
                const hbg::Vec2 uv((static_cast<float>(x) + 0.5f) / static_cast<float>(native_size),
                                   (static_cast<float>(y) + 0.5f) /
                                       static_cast<float>(native_size));
                PARCEL_TRY_DECL(float, value, SampleLuminance(texture, OctahedralDecode(uv)));
                light.cookieNative[static_cast<std::size_t>(y) * native_size + x] =
                    std::clamp(value, 0.0f, 1.0f);
            }
        }
    } else {
        light.cookieNativeWidth = NextCookiePowerOfTwo(texture.mips.front().width);
        light.cookieNativeHeight = NextCookiePowerOfTwo(texture.mips.front().height);
        light.cookieNative.assign(
            static_cast<std::size_t>(light.cookieNativeWidth) * light.cookieNativeHeight, 1.0f);
        for (std::uint32_t y = 0; y < light.cookieNativeHeight; ++y) {
            for (std::uint32_t x = 0; x < light.cookieNativeWidth; ++x) {
                const hbg::Vec2 uv(
                    (static_cast<float>(x) + 0.5f) / static_cast<float>(light.cookieNativeWidth),
                    (static_cast<float>(y) + 0.5f) / static_cast<float>(light.cookieNativeHeight));
                PARCEL_TRY_DECL(float, value, SampleLuminance(texture, uv));
                light.cookieNative[static_cast<std::size_t>(y) * light.cookieNativeWidth + x] =
                    std::clamp(value, 0.0f, 1.0f);
            }
        }
    }
    return {};
}

float SampleIesProfile(const parcel::assets::IesProfileAsset& profile, float vertical_degrees,
                       float horizontal_degrees) {
    const std::uint32_t vertical_count = static_cast<std::uint32_t>(profile.vertical_angles.size());
    const std::uint32_t horizontal_count =
        static_cast<std::uint32_t>(profile.horizontal_angles.size());

    std::uint32_t vertical_upper = 0;
    while (vertical_upper + 1 < vertical_count &&
           profile.vertical_angles[vertical_upper + 1] < vertical_degrees) {
        ++vertical_upper;
    }
    const std::uint32_t vertical_lower = vertical_upper;
    const std::uint32_t vertical_next = std::min(vertical_upper + 1, vertical_count - 1);
    const float vertical_span =
        profile.vertical_angles[vertical_next] - profile.vertical_angles[vertical_lower];
    const float vertical_t =
        vertical_span > 1.0e-5f
            ? std::clamp((vertical_degrees - profile.vertical_angles[vertical_lower]) /
                             vertical_span,
                         0.0f, 1.0f)
            : 0.0f;

    float wrapped = horizontal_degrees;
    while (wrapped < 0.0f) {
        wrapped += 360.0f;
    }
    while (wrapped >= 360.0f) {
        wrapped -= 360.0f;
    }

    std::uint32_t horizontal_upper = horizontal_count - 1;
    std::uint32_t horizontal_next = 0;
    float horizontal_t = 0.0f;
    if (horizontal_count > 1) {
        horizontal_upper = 0;
        while (horizontal_upper + 1 < horizontal_count &&
               profile.horizontal_angles[horizontal_upper + 1] <= wrapped) {
            ++horizontal_upper;
        }
        horizontal_next = (horizontal_upper + 1) % horizontal_count;
        const float lower = profile.horizontal_angles[horizontal_upper];
        const float upper =
            profile.horizontal_angles[horizontal_next] + (horizontal_next == 0 ? 360.0f : 0.0f);
        const float span = upper - lower;
        horizontal_t = span > 1.0e-5f ? std::clamp((wrapped - lower) / span, 0.0f, 1.0f) : 0.0f;
    }

    const auto value_at = [&profile, horizontal_count](std::uint32_t vertical,
                                                       std::uint32_t horizontal) {
        return profile.values[static_cast<std::size_t>(vertical) * horizontal_count + horizontal];
    };
    const float low = value_at(vertical_lower, horizontal_upper) * (1.0f - horizontal_t) +
                      value_at(vertical_lower, horizontal_next) * horizontal_t;
    const float high = value_at(vertical_next, horizontal_upper) * (1.0f - horizontal_t) +
                       value_at(vertical_next, horizontal_next) * horizontal_t;
    return low + (high - low) * vertical_t;
}

Result<void> ApplyIesProfile(const parcel::assets::IesProfileAsset& profile,
                             hbg::render::Light& light) {
    auto valid = parcel::assets::ValidateIesProfile(profile);
    if (!valid) {
        return std::move(valid).TakeError();
    }
    const std::uint32_t vertical_count = static_cast<std::uint32_t>(profile.vertical_angles.size());
    const std::uint32_t horizontal_count =
        static_cast<std::uint32_t>(profile.horizontal_angles.size());
    float maximum = 0.0f;
    for (float value : profile.values) {
        maximum = std::max(maximum, value);
    }

    if (light.type == hbg::render::LightType::Point) {
        constexpr std::uint32_t kLegacySize = hbg::render::kCookieLegacySize;
        const bool had_cookie = light.cookieType == hbg::render::LightCookieType::PointOctahedral;
        if (!had_cookie) {
            light.cookieMode = hbg::render::LightCookieMode::Clamp;
            light.cookieType = hbg::render::LightCookieType::PointOctahedral;
            light.cookieSamples.fill(1.0f);
        }
        for (std::uint32_t y = 0; y < kLegacySize; ++y) {
            for (std::uint32_t x = 0; x < kLegacySize; ++x) {
                const hbg::Vec2 uv((static_cast<float>(x) + 0.5f) / static_cast<float>(kLegacySize),
                                   (static_cast<float>(y) + 0.5f) /
                                       static_cast<float>(kLegacySize));
                const hbg::Vec3 direction = OctahedralDecodeDirection(uv);
                const float vertical =
                    std::acos(std::clamp(direction.y, -1.0f, 1.0f)) * 57.2957795131f;
                float horizontal = std::atan2(direction.x, direction.z) * 57.2957795131f;
                if (horizontal < 0.0f) {
                    horizontal += 360.0f;
                }
                const float sample = std::clamp(
                    SampleIesProfile(profile, vertical, horizontal) / maximum, 0.0f, 1.0f);
                light.cookieSamples[static_cast<std::size_t>(y) * kLegacySize + x] *= sample;
            }
        }

        if (light.cookieNativeWidth == 0 || light.cookieNativeHeight == 0) {
            light.cookieNativeWidth = 64;
            light.cookieNativeHeight = 64;
            light.cookieNative.assign(64U * 64U, 1.0f);
        }
        for (std::uint32_t y = 0; y < light.cookieNativeHeight; ++y) {
            for (std::uint32_t x = 0; x < light.cookieNativeWidth; ++x) {
                const hbg::Vec2 uv(
                    (static_cast<float>(x) + 0.5f) / static_cast<float>(light.cookieNativeWidth),
                    (static_cast<float>(y) + 0.5f) / static_cast<float>(light.cookieNativeHeight));
                const hbg::Vec3 direction = OctahedralDecodeDirection(uv);
                const float vertical =
                    std::acos(std::clamp(direction.y, -1.0f, 1.0f)) * 57.2957795131f;
                float horizontal = std::atan2(direction.x, direction.z) * 57.2957795131f;
                if (horizontal < 0.0f) {
                    horizontal += 360.0f;
                }
                const float sample = std::clamp(
                    SampleIesProfile(profile, vertical, horizontal) / maximum, 0.0f, 1.0f);
                light.cookieNative[static_cast<std::size_t>(y) * light.cookieNativeWidth + x] *=
                    sample;
            }
        }
        light.hasIes = true;
        return {};
    }

    float slice_maximum = 0.0f;
    for (std::uint32_t index = 0; index < vertical_count; ++index) {
        slice_maximum = std::max(
            slice_maximum, profile.values[static_cast<std::size_t>(index) * horizontal_count]);
    }
    if (slice_maximum <= 0.0f) {
        light.hasIes = false;
        light.iesCut = 0.0f;
        return {};
    }

    for (std::uint32_t sample = 0; sample < hbg::render::kLightIesSampleCount; ++sample) {
        const float angle = 90.0f * static_cast<float>(sample) /
                            static_cast<float>(hbg::render::kLightIesSampleCount - 1);
        const float value = SampleIesProfile(profile, angle, 0.0f);
        light.iesSamples[sample] = std::clamp(value / slice_maximum, 0.0f, 1.0f);
    }
    std::uint32_t last_visible = 0;
    for (std::uint32_t sample = 0; sample < hbg::render::kLightIesSampleCount; ++sample) {
        if (light.iesSamples[sample] > 0.001f) {
            last_visible = sample;
        }
    }
    const float cutoff_angle = 90.0f * static_cast<float>(last_visible) /
                               static_cast<float>(hbg::render::kLightIesSampleCount - 1);
    light.iesCut = std::cos(cutoff_angle * 3.14159265358979323846f / 180.0f);
    light.hasIes = true;
    return {};
}

} // namespace

Result<std::shared_ptr<parcel::assets::AssetObject>>
SceneRenderBridge::ResolveAsset(AssetGuid guid) const {
    if (!resolver_) {
        return MakeError(ErrorCode::InvalidArgument, "scene asset resolver is not set");
    }
    auto asset = resolver_(guid);
    if (!asset) {
        return std::move(asset).TakeError();
    }
    if (asset.Value() == nullptr) {
        return MakeError(ErrorCode::NotFound, "scene asset resolver returned null");
    }
    return asset.Value();
}

Result<int> SceneRenderBridge::ResolveImage(AssetGuid guid, hbg::render::RenderScene& scene) {
    if (guid.IsZero()) {
        return -1;
    }
    const auto existing = image_indices_.find(guid);
    if (existing != image_indices_.end()) {
        return existing->second;
    }

    auto asset = ResolveTextureAsset(resolver_, guid);
    if (!asset) {
        return std::move(asset).TakeError();
    }
    const auto texture = asset.Value();
    if (texture->mips.empty() || texture->mips.front().width == 0 ||
        texture->mips.front().height == 0) {
        return MakeError(ErrorCode::InvalidArgument, "image texture has no valid mip");
    }

    auto materialized = MaterializeTexture(*texture, guid.ToHex());
    if (!materialized) {
        return std::move(materialized).TakeError();
    }
    const MaterializedTexture& converted = materialized.Value();
    if (!converted.storage) {
        return MakeError(ErrorCode::Internal, "materialized image storage is missing");
    }

    hbg::asset::ImageAsset image;
    image.name = guid.ToHex();
    image.width = converted.desc.width;
    image.height = converted.desc.height;
    image.mipCount = converted.desc.mipCount;
    image.format = converted.desc.format;
    image.pixels.resize(converted.storage->size());
    if (!image.pixels.empty()) {
        std::memcpy(image.pixels.data(), converted.storage->data(), image.pixels.size());
    }
    const int index = static_cast<int>(scene.images.size());
    scene.images.push_back(std::move(image));
    image_indices_.emplace(guid, index);
    image_guids_.push_back(guid);
    return index;
}

Result<void> SceneRenderBridge::Update(const ::entt::registry& registry,
                                       hbg::render::RenderScene& scene) {
    hbg::render::RenderScene rebuilt;
    image_indices_.clear();
    image_guids_.clear();
    camera_states_.clear();
    light_texture_states_.clear();

    std::unordered_map<std::uint64_t, ::entt::entity> entities;
    for (const auto entity : registry.view<const parcel_entt::SceneEntityComponent>()) {
        const auto& tag = registry.get<const parcel_entt::SceneEntityComponent>(entity);
        if (!entities.emplace(tag.id.value, entity).second) {
            return MakeError(ErrorCode::Corrupt, "duplicate scene entity id in registry");
        }
    }

    auto node_map = BuildSceneNodes(registry, entities, rebuilt.nodes);
    if (!node_map) {
        return std::move(node_map).TakeError();
    }
    const std::unordered_map<std::uint64_t, int>& node_indices = node_map.Value();

    std::unordered_map<std::uint64_t, int> scene_node_indices;
    scene_node_indices.reserve(entities.size());
    for (const auto& [id, entity] : entities) {
        const auto node = node_indices.find(EntityKey(entity));
        if (node != node_indices.end()) {
            scene_node_indices.emplace(id, node->second);
        }
    }

    std::unordered_map<AssetGuid, int, parcel::core::Guid128Hash> material_cache;
    std::unordered_map<std::string, int> mesh_cache;
    int fallback_material_index = -1;

    std::vector<::entt::entity> player_entities;
    for (const auto entity : registry.view<const AnimationPlayerComponent>()) {
        player_entities.push_back(entity);
    }
    std::sort(player_entities.begin(), player_entities.end(),
              [&](::entt::entity lhs, ::entt::entity rhs) {
                  return EntityOrderLess(registry, lhs, rhs);
              });

    std::unordered_set<AssetGuid, parcel::core::Guid128Hash> animation_guids;
    for (const auto entity : player_entities) {
        const auto& player = registry.get<const AnimationPlayerComponent>(entity);
        for (const AssetGuid& guid : player.clips) {
            if (guid.IsZero() || !animation_guids.insert(guid).second) {
                continue;
            }
            auto asset =
                ResolveTypedAsset(resolver_, guid, parcel::assets::kAnimationClipAssetType);
            if (!asset) {
                return std::move(asset).TakeError();
            }
            const auto clip =
                std::static_pointer_cast<parcel::assets::AnimationClipAsset>(asset.Value());
            PARCEL_TRY(parcel::assets::ValidateAnimationClip(*clip));

            hbg::asset::AnimationClip converted;
            converted.name = clip->name;
            converted.duration = clip->duration;
            converted.channels.reserve(clip->channels.size());
            for (const parcel::assets::AnimationChannel& source : clip->channels) {
                const auto target = scene_node_indices.find(source.target.value);
                if (target == scene_node_indices.end()) {
                    return MakeError(ErrorCode::Corrupt,
                                     "animation channel targets an unknown scene node");
                }
                hbg::asset::AnimationChannel channel;
                channel.node = static_cast<std::uint32_t>(target->second);
                channel.path = ToHbg(source.path);
                channel.interpolation = ToHbg(source.interpolation);
                channel.targetIndex = source.target_index;
                channel.times = source.times;
                channel.values = ToHbg(source.values);
                channel.inTangents = ToHbg(source.in_tangents);
                channel.outTangents = ToHbg(source.out_tangents);
                converted.channels.push_back(std::move(channel));
            }
            rebuilt.animations.push_back(std::move(converted));
        }
    }

    const auto view = registry.view<const TransformComponent>();
    for (const auto entity : view) {
        auto world = ComputeWorld(registry, entity, entities, 0);
        if (!world) {
            return std::move(world).TakeError();
        }

        const auto node = node_indices.find(EntityKey(entity));
        if (node == node_indices.end()) {
            return MakeError(ErrorCode::Corrupt, "scene entity has no render node");
        }
        const int node_index = node->second;

        const std::string name = EntityName(registry, entity);
        if (const auto* camera = registry.try_get<CameraComponent>(entity)) {
            hbg::render::Camera converted;
            converted.name = name;
            converted.nodeIndex = node_index;
            converted.projection = static_cast<hbg::render::Camera::Projection>(camera->projection);
            converted.world = world.Value();
            converted.yfov = camera->yfov;
            converted.aspect = camera->aspect;
            converted.nearClip = camera->near_clip;
            converted.farClip = camera->far_clip;
            converted.xmag = camera->xmag;
            converted.ymag = camera->ymag;
            rebuilt.cameras.push_back(std::move(converted));
            camera_states_.push_back(
                CameraState{camera->clear_color, camera->culling_mask, camera->priority});
        }

        if (const auto* light = registry.try_get<LightComponent>(entity)) {
            hbg::render::Light converted;
            converted.name = name;
            converted.nodeIndex = node_index;
            converted.type = static_cast<hbg::render::LightType>(light->type);
            converted.color = ToHbg(light->color);
            converted.intensity = light->intensity;
            converted.range = light->range;
            converted.innerConeAngle = light->inner_cone_angle;
            converted.outerConeAngle = light->outer_cone_angle;
            converted.areaShape = static_cast<hbg::render::LightAreaShape>(light->area_shape);
            converted.areaWidth = light->area_width;
            converted.areaHeight = light->area_height;
            converted.areaShadowCone = light->area_shadow_cone;
            converted.world = world.Value();
            converted.castShadows = light->cast_shadows;
            converted.shadowConfigured = light->shadow_configured;
            converted.lightLayerMask = light->light_layer_mask;
            converted.cookieMode = static_cast<hbg::render::LightCookieMode>(light->cookie_mode);
            converted.cookieType = static_cast<hbg::render::LightCookieType>(light->cookie_type);
            converted.cookieScale = ToHbg(light->cookie_scale);
            converted.cookieOffset = ToHbg(light->cookie_offset);
            converted.iesCut = light->ies_cut;
            converted.hasIes = light->has_ies;
            if (!light->cookie.IsZero()) {
                auto cookie_texture = ResolveTextureAsset(resolver_, light->cookie);
                if (!cookie_texture) {
                    return std::move(cookie_texture).TakeError();
                }
                if (converted.cookieMode == hbg::render::LightCookieMode::None) {
                    converted.cookieMode = hbg::render::LightCookieMode::Clamp;
                }
                if (converted.cookieType == hbg::render::LightCookieType::None) {
                    converted.cookieType = converted.type == hbg::render::LightType::Point
                                               ? hbg::render::LightCookieType::PointOctahedral
                                               : hbg::render::LightCookieType::Spot2D;
                }
                auto loaded = LoadCookieSamples(*cookie_texture.Value(), converted);
                if (!loaded) {
                    return std::move(loaded).TakeError();
                }
            }
            if (light->has_ies) {
                if (light->ies.IsZero()) {
                    return MakeError(ErrorCode::Corrupt, "light has IES enabled without a profile");
                }
                auto profile_asset =
                    ResolveTypedAsset(resolver_, light->ies, parcel::assets::kIesProfileAssetType);
                if (!profile_asset) {
                    return std::move(profile_asset).TakeError();
                }
                auto applied =
                    ApplyIesProfile(*std::static_pointer_cast<parcel::assets::IesProfileAsset>(
                                        profile_asset.Value()),
                                    converted);
                if (!applied) {
                    return std::move(applied).TakeError();
                }
            }
            converted.shadow.dimmer = light->shadow.dimmer;
            converted.shadow.fadeDistance = light->shadow.fade_distance;
            converted.shadow.nearPlane = light->shadow.near_plane;
            converted.shadow.resolution = light->shadow.resolution;
            converted.shadow.tint = ToHbg(light->shadow.tint);
            converted.shadow.penumbraTint = light->shadow.penumbra_tint;
            converted.shadow.updateMode =
                static_cast<hbg::render::ShadowUpdateMode>(light->shadow.update_mode);
            converted.shadow.contactShadows = light->shadow.contact_shadows;
            converted.shadow.softness = light->shadow.softness;
            converted.shadow.blockerSamples = light->shadow.blocker_samples;
            converted.shadow.filterSamples = light->shadow.filter_samples;
            LightTextureState textures;
            PARCEL_TRY_ASSIGN(textures.cookie_image, ResolveImage(light->cookie, rebuilt));
            light_texture_states_.push_back(textures);
            rebuilt.lights.push_back(std::move(converted));
        }

        if (const auto* renderer = registry.try_get<MeshRendererComponent>(entity)) {
            if (!renderer->mesh.IsZero()) {
                auto mesh_asset =
                    ResolveTypedAsset(resolver_, renderer->mesh, parcel::assets::kMeshAssetType);
                if (!mesh_asset) {
                    return std::move(mesh_asset).TakeError();
                }
                auto mesh = MaterializeMesh(
                    *std::static_pointer_cast<parcel::assets::MeshAsset>(mesh_asset.Value()));
                if (!mesh) {
                    return std::move(mesh).TakeError();
                }

                std::vector<int> material_indices;
                for (const AssetGuid& material_guid : renderer->materials) {
                    if (material_guid.IsZero()) {
                        continue;
                    }
                    const auto cached = material_cache.find(material_guid);
                    if (cached != material_cache.end()) {
                        material_indices.push_back(cached->second);
                        continue;
                    }
                    auto material_asset = ResolveTypedAsset(resolver_, material_guid,
                                                            parcel::assets::kMaterialAssetType);
                    if (!material_asset) {
                        return std::move(material_asset).TakeError();
                    }
                    auto material = MaterializeMaterial(
                        *std::static_pointer_cast<parcel::assets::MaterialAsset>(
                            material_asset.Value()),
                        [this, &rebuilt](AssetGuid guid) { return ResolveImage(guid, rebuilt); });
                    if (!material) {
                        return std::move(material).TakeError();
                    }
                    const int material_index = static_cast<int>(rebuilt.materials.size());
                    rebuilt.materials.push_back(std::move(material).Value());
                    material_cache.emplace(material_guid, material_index);
                    material_indices.push_back(material_index);
                }

                auto fallback_index = [&]() {
                    if (fallback_material_index < 0) {
                        hbg::asset::Material fallback;
                        fallback.name = "DefaultMaterial";
                        fallback_material_index = static_cast<int>(rebuilt.materials.size());
                        rebuilt.materials.push_back(std::move(fallback));
                    }
                    return fallback_material_index;
                };

                hbg::asset::Mesh converted = std::move(mesh).Value();
                for (std::size_t submesh_index = 0; submesh_index < converted.submeshes.size();
                     ++submesh_index) {
                    hbg::asset::SubMesh& submesh = converted.submeshes[submesh_index];
                    int local_material = static_cast<int>(submesh.materialIndex);
                    if (!renderer->submesh_materials.empty() &&
                        submesh_index < renderer->submesh_materials.size()) {
                        local_material = renderer->submesh_materials[submesh_index];
                    }
                    if (local_material < 0 ||
                        static_cast<std::size_t>(local_material) >= material_indices.size()) {
                        submesh.materialIndex = static_cast<std::uint32_t>(fallback_index());
                    } else {
                        submesh.materialIndex =
                            static_cast<std::uint32_t>(material_indices[local_material]);
                    }
                }
                if (converted.submeshes.empty() && material_indices.empty()) {
                    fallback_index();
                }

                std::string mesh_key = renderer->mesh.ToHex();
                for (const hbg::asset::SubMesh& submesh : converted.submeshes) {
                    mesh_key += ':' + std::to_string(submesh.materialIndex);
                }
                const bool share_mesh =
                    converted.skinVertices.empty() && converted.morphTargets.empty();
                int mesh_index = -1;
                if (share_mesh) {
                    const auto cached = mesh_cache.find(mesh_key);
                    if (cached != mesh_cache.end()) {
                        mesh_index = cached->second;
                    }
                }
                if (mesh_index < 0) {
                    mesh_index = static_cast<int>(rebuilt.meshes.size());
                    if (share_mesh) {
                        mesh_cache.emplace(mesh_key, mesh_index);
                    }
                }
                const hbg::Bounds local_bounds = converted.bounds;

                int skin_index = -1;
                if (!converted.skinVertices.empty()) {
                    const auto* skin = registry.try_get<SkinComponent>(entity);
                    if (skin == nullptr) {
                        return MakeError(ErrorCode::Corrupt,
                                         "mesh has skin vertices without a skin component");
                    }
                    if (skin->joints.empty() ||
                        skin->joints.size() != skin->inverse_bind_matrices.size()) {
                        return MakeError(ErrorCode::Corrupt, "skin component data is invalid");
                    }
                    for (const hbg::asset::SkinVertex& vertex : converted.skinVertices) {
                        const std::size_t begin = vertex.influenceOffset;
                        const std::size_t end =
                            begin + static_cast<std::size_t>(vertex.influenceCount);
                        if (begin > converted.skinInfluences.size() ||
                            end > converted.skinInfluences.size()) {
                            return MakeError(ErrorCode::Corrupt,
                                             "skin vertex influence range is invalid");
                        }
                        for (std::size_t index = begin; index < end; ++index) {
                            const hbg::asset::SkinInfluence& influence =
                                converted.skinInfluences[index];
                            if (influence.weight > 0.0f && influence.joint >= skin->joints.size()) {
                                return MakeError(ErrorCode::Corrupt,
                                                 "skin vertex references an unknown joint");
                            }
                        }
                    }
                    hbg::asset::Skin converted_skin;
                    converted_skin.name = name.empty() ? "Skin" : name + ".Skin";
                    converted_skin.joints.reserve(skin->joints.size());
                    converted_skin.inverseBindMatrices.reserve(skin->joints.size());
                    for (std::size_t joint_index = 0; joint_index < skin->joints.size();
                         ++joint_index) {
                        const auto target =
                            scene_node_indices.find(skin->joints[joint_index].value);
                        if (target == scene_node_indices.end()) {
                            return MakeError(ErrorCode::Corrupt,
                                             "skin joint targets an unknown scene node");
                        }
                        converted_skin.joints.push_back(static_cast<std::uint32_t>(target->second));
                        converted_skin.inverseBindMatrices.push_back(
                            ToHbg(skin->inverse_bind_matrices[joint_index]));
                    }
                    skin_index = static_cast<int>(rebuilt.skins.size());
                    rebuilt.skins.push_back(std::move(converted_skin));
                } else if (registry.try_get<SkinComponent>(entity) != nullptr) {
                    return MakeError(ErrorCode::Corrupt,
                                     "skin component has no matching mesh skin vertices");
                }

                std::vector<float> morph_weights;
                if (!converted.morphTargets.empty()) {
                    if (const auto* morph = registry.try_get<MorphComponent>(entity);
                        morph != nullptr) {
                        if (morph->weights.size() != converted.morphTargets.size()) {
                            return MakeError(ErrorCode::Corrupt,
                                             "morph component weight count does not match mesh");
                        }
                        morph_weights = morph->weights;
                    } else if (!converted.morphWeights.empty()) {
                        morph_weights = converted.morphWeights;
                    } else {
                        morph_weights.assign(converted.morphTargets.size(), 0.0f);
                    }
                    for (float weight : morph_weights) {
                        if (!std::isfinite(weight)) {
                            return MakeError(ErrorCode::Corrupt, "morph weight is invalid");
                        }
                    }
                    rebuilt.nodes[node_index].weights = morph_weights;
                    rebuilt.nodes[node_index].baseWeights = morph_weights;
                } else if (registry.try_get<MorphComponent>(entity) != nullptr) {
                    return MakeError(ErrorCode::Corrupt,
                                     "morph component has no matching mesh morph targets");
                }

                hbg::render::RenderObject object;
                object.name = name;
                object.nodeIndex = node_index;
                object.skinIndex = skin_index;
                object.meshIndex = static_cast<std::uint32_t>(mesh_index);
                object.morphWeights = std::move(morph_weights);
                object.world = world.Value();
                object.prevWorld = world.Value();
                object.worldBounds = skin_index >= 0
                                         ? local_bounds
                                         : hbg::transformBounds(object.world, local_bounds);
                object.staticShadowCaster = skin_index < 0;
                object.sortingLayer = renderer->sorting_layer;
                object.sortingOrder = renderer->sorting_order;
                object.rendererPriority = renderer->renderer_priority;
                object.lightLayerMask = renderer->light_layer_mask;
                object.decalLayerMask = renderer->decal_layer_mask;
                rebuilt.objects.push_back(std::move(object));
                if (mesh_index == static_cast<int>(rebuilt.meshes.size())) {
                    rebuilt.meshes.push_back(std::move(converted));
                }
            }
        }

        if (const auto* decal = registry.try_get<DecalComponent>(entity)) {
            hbg::render::Decal converted;
            converted.name = name;
            PARCEL_TRY_ASSIGN(converted.albedoImage, ResolveImage(decal->albedo, rebuilt));
            PARCEL_TRY_ASSIGN(converted.normalImage, ResolveImage(decal->normal, rebuilt));
            PARCEL_TRY_ASSIGN(converted.maskImage, ResolveImage(decal->mask, rebuilt));
            converted.position = ToHbg(decal->position);
            converted.rotation = ToHbg(decal->rotation);
            converted.size = ToHbg(decal->size);
            converted.baseColor = ToHbg(decal->base_color);
            converted.opacity = decal->opacity;
            converted.angleFade = decal->angle_fade;
            converted.normalBlend = decal->normal_blend;
            converted.metallic = decal->metallic;
            converted.ambientOcclusion = decal->ambient_occlusion;
            converted.roughness = decal->roughness;
            converted.layerMask = decal->layer_mask;
            converted.channelMask = decal->channel_mask;
            converted.enabled = decal->enabled;
            rebuilt.decals.push_back(std::move(converted));
        }

        if (const auto* probe = registry.try_get<ReflectionProbeComponent>(entity)) {
            hbg::render::ReflectionProbe converted;
            converted.name = name;
            PARCEL_TRY_ASSIGN(converted.image, ResolveImage(probe->image, rebuilt));
            converted.position = ToHbg(probe->position);
            converted.rotation = ToHbg(probe->rotation);
            converted.boxExtents = ToHbg(probe->box_extents);
            converted.blendDistance = probe->blend_distance;
            converted.intensity = probe->intensity;
            converted.enabled = probe->enabled;
            rebuilt.probes.push_back(std::move(converted));
        }

        if (const auto* fog = registry.try_get<LocalFogVolumeComponent>(entity)) {
            hbg::render::LocalFogVolume converted;
            converted.name = name;
            converted.enabled = fog->enabled;
            converted.shape = static_cast<hbg::render::LocalFogShape>(fog->shape);
            converted.center = ToHbg(fog->center);
            converted.rotation = ToHbg(fog->rotation);
            converted.size = ToHbg(fog->size);
            converted.albedo = ToHbg(fog->albedo);
            converted.meanFreePath = fog->mean_free_path;
            converted.anisotropy = fog->anisotropy;
            converted.positiveFade = ToHbg(fog->positive_fade);
            converted.negativeFade = ToHbg(fog->negative_fade);
            converted.invertFade = fog->invert_fade;
            converted.falloff = static_cast<hbg::render::LocalFogFalloff>(fog->falloff);
            converted.distanceFadeStart = fog->distance_fade_start;
            converted.distanceFadeEnd = fog->distance_fade_end;
            PARCEL_TRY_ASSIGN(converted.maskImage, ResolveImage(fog->mask, rebuilt));
            converted.maskTiling = ToHbg(fog->mask_tiling);
            converted.maskScroll = ToHbg(fog->mask_scroll);
            converted.maskPlane = static_cast<hbg::render::LocalFogMaskPlane>(fog->mask_plane);
            converted.maskIntensity = fog->mask_intensity;
            converted.maskInvert = fog->mask_invert;
            rebuilt.localFogVolumes.push_back(std::move(converted));
        }
    }

    if (!rebuilt.bounds.valid()) {
        rebuilt.bounds.min = hbg::Vec3{};
        rebuilt.bounds.max = hbg::Vec3{};
    }

    scene = std::move(rebuilt);
    dirty_ = false;
    return {};
}

Result<void> SceneRenderBridge::UpdateIfDirty(const ::entt::registry& registry,
                                              hbg::render::RenderScene& scene) {
    if (!dirty_) {
        return {};
    }
    return Update(registry, scene);
}

} // namespace parcel_hbg
