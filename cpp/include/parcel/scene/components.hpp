#pragma once

#include "parcel/core/guid.hpp"
#include "parcel/math.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace parcel::scene {

using ComponentTypeId = std::uint32_t;
using core::AssetGuid;
using EntityId = std::uint64_t;

inline constexpr ComponentTypeId kTransformComponentType = 1;
inline constexpr ComponentTypeId kCameraComponentType = 2;
inline constexpr ComponentTypeId kLightComponentType = 3;
inline constexpr ComponentTypeId kMeshRendererComponentType = 4;
inline constexpr ComponentTypeId kDecalComponentType = 5;
inline constexpr ComponentTypeId kReflectionProbeComponentType = 6;
inline constexpr ComponentTypeId kLocalFogVolumeComponentType = 7;
inline constexpr ComponentTypeId kAudioSourceComponentType = 8;
inline constexpr ComponentTypeId kAudioListenerComponentType = 9;
inline constexpr ComponentTypeId kRigidBodyComponentType = 10;
inline constexpr ComponentTypeId kColliderComponentType = 11;
inline constexpr ComponentTypeId kScriptComponentType = 12;
inline constexpr ComponentTypeId kAnimationPlayerComponentType = 13;
inline constexpr ComponentTypeId kSkinComponentType = 14;
inline constexpr ComponentTypeId kMorphComponentType = 15;

inline constexpr std::uint32_t kStandardComponentSchemaVersion = 1;
inline constexpr std::uint32_t kMaxMorphWeights = 256;

struct SceneEntityId {
    EntityId value = 0;

    [[nodiscard]] bool IsRoot() const noexcept { return value == 0; }
    friend bool operator==(const SceneEntityId&, const SceneEntityId&) = default;
    friend bool operator<(const SceneEntityId&, const SceneEntityId&) = default;
};

struct TransformData {
    Vec3 position{};
    Quat rotation = Quat::Identity();
    Vec3 scale{1.0f, 1.0f, 1.0f};

    friend bool operator==(const TransformData&, const TransformData&) = default;
};

struct TransformComponent {
    Vec3 position{};
    Quat rotation = Quat::Identity();
    Vec3 scale{1.0f, 1.0f, 1.0f};
    SceneEntityId parent;

    friend bool operator==(const TransformComponent&, const TransformComponent&) = default;
};

struct CameraComponent {
    enum class Projection : std::uint8_t { Perspective, Orthographic };

    Projection projection = Projection::Perspective;
    float yfov = 1.0471975512f;
    float aspect = 1.7777778f;
    float near_clip = 0.1f;
    float far_clip = 1000.0f;
    float xmag = 1.0f;
    float ymag = 1.0f;
    Vec4 clear_color{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t culling_mask = 0xffffffffU;
    std::int32_t priority = 0;

    friend bool operator==(const CameraComponent&, const CameraComponent&) = default;
};

enum class LightType : std::uint8_t { Directional, Point, Spot, Area };
enum class LightCookieMode : std::uint8_t { None, Clamp, Repeat };
enum class LightCookieType : std::uint8_t { None, Spot2D, PointOctahedral };
enum class LightAreaShape : std::uint8_t { Rectangle, Tube, Disc };
enum class ShadowUpdateMode : std::uint8_t { EveryFrame = 0, OnEnable = 1, OnDemand = 2 };

struct LightShadowSettings {
    float dimmer = 1.0f;
    float fade_distance = 10000.0f;
    float near_plane = 0.1f;
    std::uint32_t resolution = 0;
    Vec3 tint{};
    bool penumbra_tint = false;
    ShadowUpdateMode update_mode = ShadowUpdateMode::EveryFrame;
    bool contact_shadows = true;
    float softness = 0.5f;
    std::uint32_t blocker_samples = 16;
    std::uint32_t filter_samples = 24;

    friend bool operator==(const LightShadowSettings&, const LightShadowSettings&) = default;
};

struct LightComponent {
    LightType type = LightType::Point;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    float inner_cone_angle = 0.0f;
    float outer_cone_angle = 0.78539816339f;
    LightAreaShape area_shape = LightAreaShape::Rectangle;
    float area_width = 1.0f;
    float area_height = 1.0f;
    float area_shadow_cone = 120.0f;
    bool cast_shadows = false;
    bool shadow_configured = false;
    std::uint16_t light_layer_mask = 0xffffU;
    LightCookieMode cookie_mode = LightCookieMode::None;
    LightCookieType cookie_type = LightCookieType::None;
    Vec2 cookie_scale{1.0f, 1.0f};
    Vec2 cookie_offset{};
    AssetGuid cookie;
    AssetGuid ies;
    bool has_ies = false;
    float ies_cut = 0.0f;
    LightShadowSettings shadow;

    friend bool operator==(const LightComponent&, const LightComponent&) = default;
};

struct MeshRendererComponent {
    AssetGuid mesh;
    std::vector<AssetGuid> materials;
    std::vector<std::int32_t> submesh_materials;
    bool cast_shadows = true;
    bool receive_shadows = true;
    std::int32_t sorting_layer = 0;
    std::int32_t sorting_order = 0;
    std::int32_t renderer_priority = 0;
    std::uint16_t light_layer_mask = 0xffffU;
    std::uint8_t decal_layer_mask = 0x7fU;
    AssetGuid lightmap_atlas;
    Vec4 lightmap_st{};
    Vec2 lightmap_size{};

    friend bool operator==(const MeshRendererComponent&, const MeshRendererComponent&) = default;
};

struct AnimationPlayerComponent {
    std::vector<AssetGuid> clips;

    friend bool operator==(const AnimationPlayerComponent&,
                           const AnimationPlayerComponent&) = default;
};

struct SkinComponent {
    std::vector<SceneEntityId> joints;
    std::vector<Mat4> inverse_bind_matrices;

    friend bool operator==(const SkinComponent&, const SkinComponent&) = default;
};

struct MorphComponent {
    std::vector<float> weights;

    friend bool operator==(const MorphComponent&, const MorphComponent&) = default;
};

struct DecalComponent {
    AssetGuid albedo;
    AssetGuid normal;
    AssetGuid mask;
    Vec3 position{};
    Vec3 rotation{};
    Vec3 size{1.0f, 1.0f, 1.0f};
    Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
    float opacity = 1.0f;
    float angle_fade = 0.0f;
    float normal_blend = 0.0f;
    float metallic = 0.0f;
    float ambient_occlusion = 1.0f;
    float roughness = 0.5f;
    std::uint32_t layer_mask = 0x7fU;
    std::uint32_t channel_mask = 0x1fU;
    bool enabled = true;

    friend bool operator==(const DecalComponent&, const DecalComponent&) = default;
};

struct ReflectionProbeComponent {
    AssetGuid image;
    Vec3 position{};
    Vec3 rotation{};
    Vec3 box_extents{1.0f, 1.0f, 1.0f};
    float blend_distance = 0.1f;
    float intensity = 1.0f;
    bool enabled = true;

    friend bool operator==(const ReflectionProbeComponent&,
                           const ReflectionProbeComponent&) = default;
};

enum class LocalFogShape : std::uint8_t { Box, Sphere };
enum class LocalFogFalloff : std::uint8_t { Linear, Exponential };
enum class LocalFogMaskPlane : std::uint8_t { XY, XZ, YZ };

struct LocalFogVolumeComponent {
    bool enabled = true;
    LocalFogShape shape = LocalFogShape::Box;
    Vec3 center{};
    Vec3 rotation{};
    Vec3 size{1.0f, 1.0f, 1.0f};
    Vec3 albedo{1.0f, 1.0f, 1.0f};
    float mean_free_path = 10.0f;
    float anisotropy = 0.0f;
    Vec3 positive_fade{0.1f, 0.1f, 0.1f};
    Vec3 negative_fade{0.1f, 0.1f, 0.1f};
    bool invert_fade = false;
    LocalFogFalloff falloff = LocalFogFalloff::Linear;
    float distance_fade_start = 10000.0f;
    float distance_fade_end = 10000.0f;
    AssetGuid mask;
    Vec3 mask_tiling{1.0f, 1.0f, 1.0f};
    Vec3 mask_scroll{};
    LocalFogMaskPlane mask_plane = LocalFogMaskPlane::XZ;
    float mask_intensity = 1.0f;
    bool mask_invert = false;

    friend bool operator==(const LocalFogVolumeComponent&,
                           const LocalFogVolumeComponent&) = default;
};

enum class AudioRolloffMode : std::uint8_t { Logarithmic, Linear };

struct AudioSourceComponent {
    AssetGuid clip;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool play_on_awake = true;
    bool spatial = true;
    float min_distance = 1.0f;
    float max_distance = 500.0f;
    AudioRolloffMode rolloff = AudioRolloffMode::Logarithmic;

    friend bool operator==(const AudioSourceComponent&, const AudioSourceComponent&) = default;
};

struct AudioListenerComponent {
    bool enabled = true;

    friend bool operator==(const AudioListenerComponent&, const AudioListenerComponent&) = default;
};

enum class MotionType : std::uint8_t { Static, Kinematic, Dynamic };
enum class RigidBodyInterpolation : std::uint8_t { None, Interpolate, Extrapolate };

struct RigidBodyComponent {
    MotionType motion_type = MotionType::Dynamic;
    float mass = 1.0f;
    float drag = 0.0f;
    float angular_drag = 0.05f;
    bool use_gravity = true;
    std::uint32_t constraints = 0;
    RigidBodyInterpolation interpolation = RigidBodyInterpolation::None;

    friend bool operator==(const RigidBodyComponent&, const RigidBodyComponent&) = default;
};

enum class ColliderShape : std::uint8_t { Box, Sphere, Capsule, Mesh };

struct ColliderComponent {
    ColliderShape shape = ColliderShape::Box;
    Vec3 size{1.0f, 1.0f, 1.0f};
    Vec3 center{};
    bool trigger = false;
    float friction = 0.6f;
    float restitution = 0.0f;
    std::uint32_t layer = 0;
    std::uint32_t mask = 0xffffffffU;

    friend bool operator==(const ColliderComponent&, const ColliderComponent&) = default;
};

enum class ScriptLanguage : std::uint8_t { CSharp, Lua, Native };
enum class PropertyType : std::uint8_t { Bool, Int, Float, String, Vec2, Vec3, Vec4, Guid };

struct ScriptProperty {
    std::string name;
    PropertyType type = PropertyType::Bool;
    bool bool_value = false;
    std::int64_t int_value = 0;
    float float_value = 0.0f;
    std::string string_value;
    Vec2 vec2_value{};
    Vec3 vec3_value{};
    Vec4 vec4_value{};
    AssetGuid guid_value;

    friend bool operator==(const ScriptProperty&, const ScriptProperty&) = default;
};

struct PropertyBag {
    std::vector<ScriptProperty> properties;

    friend bool operator==(const PropertyBag&, const PropertyBag&) = default;
};

struct ScriptComponent {
    ScriptLanguage language = ScriptLanguage::CSharp;
    AssetGuid script;
    std::string class_name;
    std::string module_name;
    bool enabled = true;
    PropertyBag properties;

    friend bool operator==(const ScriptComponent&, const ScriptComponent&) = default;
};

} // namespace parcel::scene
