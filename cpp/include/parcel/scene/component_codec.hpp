#pragma once

#include "parcel/core/result.hpp"
#include "parcel/scene/components.hpp"
#include "parcel/serialize/binary.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace parcel::scene {

using core::Result;

template <typename T> struct ComponentCodec;

template <> struct ComponentCodec<TransformComponent> {
    static constexpr ComponentTypeId type_id = kTransformComponentType;
    static constexpr std::string_view type_name = "TransformComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const TransformComponent& value);
    static Result<TransformComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<CameraComponent> {
    static constexpr ComponentTypeId type_id = kCameraComponentType;
    static constexpr std::string_view type_name = "CameraComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const CameraComponent& value);
    static Result<CameraComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<LightComponent> {
    static constexpr ComponentTypeId type_id = kLightComponentType;
    static constexpr std::string_view type_name = "LightComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const LightComponent& value);
    static Result<LightComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<MeshRendererComponent> {
    static constexpr ComponentTypeId type_id = kMeshRendererComponentType;
    static constexpr std::string_view type_name = "MeshRendererComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const MeshRendererComponent& value);
    static Result<MeshRendererComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<AnimationPlayerComponent> {
    static constexpr ComponentTypeId type_id = kAnimationPlayerComponentType;
    static constexpr std::string_view type_name = "AnimationPlayerComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const AnimationPlayerComponent& value);
    static Result<AnimationPlayerComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<SkinComponent> {
    static constexpr ComponentTypeId type_id = kSkinComponentType;
    static constexpr std::string_view type_name = "SkinComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const SkinComponent& value);
    static Result<SkinComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<MorphComponent> {
    static constexpr ComponentTypeId type_id = kMorphComponentType;
    static constexpr std::string_view type_name = "MorphComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const MorphComponent& value);
    static Result<MorphComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<DecalComponent> {
    static constexpr ComponentTypeId type_id = kDecalComponentType;
    static constexpr std::string_view type_name = "DecalComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const DecalComponent& value);
    static Result<DecalComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<ReflectionProbeComponent> {
    static constexpr ComponentTypeId type_id = kReflectionProbeComponentType;
    static constexpr std::string_view type_name = "ReflectionProbeComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const ReflectionProbeComponent& value);
    static Result<ReflectionProbeComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<LocalFogVolumeComponent> {
    static constexpr ComponentTypeId type_id = kLocalFogVolumeComponentType;
    static constexpr std::string_view type_name = "LocalFogVolumeComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const LocalFogVolumeComponent& value);
    static Result<LocalFogVolumeComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<AudioSourceComponent> {
    static constexpr ComponentTypeId type_id = kAudioSourceComponentType;
    static constexpr std::string_view type_name = "AudioSourceComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const AudioSourceComponent& value);
    static Result<AudioSourceComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<AudioSourceMixComponent> {
    static constexpr ComponentTypeId type_id = kAudioSourceMixComponentType;
    static constexpr std::string_view type_name = "AudioSourceMixComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const AudioSourceMixComponent& value);
    static Result<AudioSourceMixComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<AudioReverbZoneComponent> {
    static constexpr ComponentTypeId type_id = kAudioReverbZoneComponentType;
    static constexpr std::string_view type_name = "AudioReverbZoneComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const AudioReverbZoneComponent& value);
    static Result<AudioReverbZoneComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<AudioListenerComponent> {
    static constexpr ComponentTypeId type_id = kAudioListenerComponentType;
    static constexpr std::string_view type_name = "AudioListenerComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const AudioListenerComponent& value);
    static Result<AudioListenerComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<RigidBodyComponent> {
    static constexpr ComponentTypeId type_id = kRigidBodyComponentType;
    static constexpr std::string_view type_name = "RigidBodyComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const RigidBodyComponent& value);
    static Result<RigidBodyComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<ColliderComponent> {
    static constexpr ComponentTypeId type_id = kColliderComponentType;
    static constexpr std::string_view type_name = "ColliderComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const ColliderComponent& value);
    static Result<ColliderComponent> Decode(std::span<const std::byte> payload);
};

template <> struct ComponentCodec<ScriptComponent> {
    static constexpr ComponentTypeId type_id = kScriptComponentType;
    static constexpr std::string_view type_name = "ScriptComponent";
    static constexpr std::uint32_t schema_version = kStandardComponentSchemaVersion;
    static Result<std::vector<std::byte>> Encode(const ScriptComponent& value);
    static Result<ScriptComponent> Decode(std::span<const std::byte> payload);
};

template <typename T> Result<std::vector<std::byte>> EncodeComponent(const T& value) {
    return ComponentCodec<T>::Encode(value);
}

template <typename T> Result<T> DecodeComponent(std::span<const std::byte> payload) {
    return ComponentCodec<T>::Decode(payload);
}

} // namespace parcel::scene
