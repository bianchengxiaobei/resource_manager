#include "parcel/import/sidecar_merge.hpp"

#include "parcel/import/ies_profile.hpp"
#include "parcel/import/meta.hpp"

#include "parcel/core/error.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/path.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace parcel::import {
namespace {

using core::AssetGuid;
using core::ErrorCode;
using core::MakeError;
using scene::DecalComponent;
using scene::LocalFogFalloff;
using scene::LocalFogMaskPlane;
using scene::LocalFogShape;
using scene::LocalFogVolumeComponent;
using scene::ReflectionProbeComponent;
using scene::SceneData;
using scene::SceneEntityId;

constexpr float kMinFogDistanceFadeLength = 0.00001526f;

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string ObjectContext(std::string_view kind, std::size_t index) {
    return std::string(kind) + "[" + std::to_string(index) + "]";
}

void Warn(std::vector<std::string>& warnings, std::string message) {
    warnings.push_back(std::move(message));
}

const assets::JsonValue* Find(const assets::SidecarObject& object, std::string_view key) {
    return object.data.Find(key);
}

std::optional<float> ReadFloat(const assets::SidecarObject& object, std::string_view key,
                               std::string_view context, std::vector<std::string>& warnings) {
    const assets::JsonValue* value = Find(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->IsNumber() || !std::isfinite(value->number_value) ||
        std::abs(value->number_value) > std::numeric_limits<float>::max()) {
        Warn(warnings,
             std::string(context) + "." + std::string(key) + ": expected a finite number");
        return std::nullopt;
    }
    return static_cast<float>(value->number_value);
}

std::optional<std::uint32_t> ReadUint(const assets::SidecarObject& object, std::string_view key,
                                      std::string_view context,
                                      std::vector<std::string>& warnings) {
    const assets::JsonValue* value = Find(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->IsNumber() || value->number_value < 0.0 ||
        value->number_value > std::numeric_limits<std::uint32_t>::max() ||
        std::floor(value->number_value) != value->number_value) {
        Warn(warnings,
             std::string(context) + "." + std::string(key) + ": expected an unsigned integer");
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value->number_value);
}

std::optional<bool> ReadBool(const assets::SidecarObject& object, std::string_view key,
                             std::string_view context, std::vector<std::string>& warnings) {
    const assets::JsonValue* value = Find(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->IsBool()) {
        Warn(warnings, std::string(context) + "." + std::string(key) + ": expected a boolean");
        return std::nullopt;
    }
    return value->bool_value;
}

std::optional<std::string> ReadString(const assets::SidecarObject& object, std::string_view key,
                                      std::string_view context,
                                      std::vector<std::string>& warnings) {
    const assets::JsonValue* value = Find(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->IsString()) {
        Warn(warnings, std::string(context) + "." + std::string(key) + ": expected a string");
        return std::nullopt;
    }
    return value->string_value;
}

std::optional<Vec3> ReadVec3(const assets::SidecarObject& object, std::string_view key,
                             std::string_view context, std::vector<std::string>& warnings) {
    const assets::JsonValue* value = Find(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->IsArray() || value->array_value.size() != 3) {
        Warn(warnings, std::string(context) + "." + std::string(key) +
                           ": expected an array of three numbers");
        return std::nullopt;
    }
    float components[3];
    for (std::size_t i = 0; i < 3; ++i) {
        const assets::JsonValue& component = value->array_value[i];
        if (!component.IsNumber() || !std::isfinite(component.number_value) ||
            std::abs(component.number_value) > std::numeric_limits<float>::max()) {
            Warn(warnings,
                 std::string(context) + "." + std::string(key) + ": expected three finite numbers");
            return std::nullopt;
        }
        components[i] = static_cast<float>(component.number_value);
    }
    return Vec3{components[0], components[1], components[2]};
}

std::optional<Vec4> ReadVec4(const assets::SidecarObject& object, std::string_view key,
                             std::string_view context, std::vector<std::string>& warnings) {
    const assets::JsonValue* value = Find(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->IsArray() || value->array_value.size() != 4) {
        Warn(warnings,
             std::string(context) + "." + std::string(key) + ": expected an array of four numbers");
        return std::nullopt;
    }
    float components[4];
    for (std::size_t i = 0; i < 4; ++i) {
        const assets::JsonValue& component = value->array_value[i];
        if (!component.IsNumber() || !std::isfinite(component.number_value) ||
            std::abs(component.number_value) > std::numeric_limits<float>::max()) {
            Warn(warnings,
                 std::string(context) + "." + std::string(key) + ": expected four finite numbers");
            return std::nullopt;
        }
        components[i] = static_cast<float>(component.number_value);
    }
    return Vec4{components[0], components[1], components[2], components[3]};
}

std::string DirectoryOf(std::string_view path) {
    const std::string normalized = core::path::Normalize(path);
    const auto slash = normalized.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    if (slash == 0) {
        return "/";
    }
    if (slash == 2 && normalized[1] == ':') {
        return normalized.substr(0, 2);
    }
    return normalized.substr(0, slash);
}

bool IsRegularFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

core::Result<std::string> ProjectRelative(std::string_view project_root,
                                          const std::filesystem::path& candidate) {
    std::error_code error;
    const std::filesystem::path root =
        std::filesystem::path(std::u8string(project_root.begin(), project_root.end()));
    const std::filesystem::path relative = std::filesystem::relative(candidate, root, error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to resolve sidecar asset: " + error.message());
    }
    const std::u8string value = relative.u8string();
    const std::string normalized = core::path::Normalize(
        std::string(reinterpret_cast<const char*>(value.data()), value.size()));
    if (normalized == ".." || normalized.rfind("../", 0) == 0 || normalized.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "sidecar asset escapes the project");
    }
    return normalized;
}

std::optional<std::string> ResolveRelativeFile(std::string_view value,
                                               const SidecarMergeOptions& options,
                                               std::string_view context,
                                               std::vector<std::string>& warnings,
                                               std::string_view kind) {
    if (value.empty()) {
        return std::nullopt;
    }

    const std::string normalized = core::path::Normalize(value);
    const std::filesystem::path project_root = std::filesystem::path(
        std::u8string(options.project_root.begin(), options.project_root.end()));
    const std::string source_directory_text = DirectoryOf(options.source_path);
    const std::filesystem::path source_directory(
        std::u8string(source_directory_text.begin(), source_directory_text.end()));

    std::vector<std::filesystem::path> candidates;
    if (core::path::IsAbsolute(normalized)) {
        candidates.emplace_back(std::u8string(normalized.begin(), normalized.end()));
    } else {
        candidates.push_back(source_directory /
                             std::u8string(normalized.begin(), normalized.end()));
        candidates.push_back(project_root / std::u8string(normalized.begin(), normalized.end()));
    }

    for (const std::filesystem::path& raw : candidates) {
        const std::filesystem::path candidate = raw.lexically_normal();
        if (!IsRegularFile(candidate)) {
            continue;
        }
        auto relative = ProjectRelative(options.project_root, candidate);
        if (!relative) {
            Warn(warnings, std::string(context) + ": " + relative.GetError().message);
            return std::nullopt;
        }
        return relative.Value();
    }

    Warn(warnings,
         std::string(context) + ": missing " + std::string(kind) + ": " + std::string(value));
    return std::nullopt;
}

AssetGuid ResolveTexturePath(std::string_view value, const SidecarMergeOptions& options,
                             std::string_view context, std::vector<std::string>& warnings) {
    const std::optional<std::string> relative =
        ResolveRelativeFile(value, options, context, warnings, "texture");
    if (!relative) {
        return {};
    }
    if (const TextureCanonicalAsset* canonical =
            FindCanonicalTextureByPath(options.canonical_textures, *relative);
        canonical != nullptr) {
        return canonical->asset_guid;
    }

    const std::filesystem::path project_root = std::filesystem::path(
        std::u8string(options.project_root.begin(), options.project_root.end()));
    const std::filesystem::path candidate =
        project_root / std::u8string(relative->begin(), relative->end());
    const std::u8string candidate_text = candidate.u8string();
    const std::string_view candidate_view(reinterpret_cast<const char*>(candidate_text.data()),
                                          candidate_text.size());
    std::string content_key;
    if (const auto hash = core::Sha256File(candidate_view); hash) {
        content_key = hash.Value().ToHex();
    }
    core::AssetGuid guid = DefaultTextureAssetGuid(*relative);
    if (options.texture_aliases != nullptr && !content_key.empty()) {
        const auto existing = options.texture_aliases->find(content_key);
        if (existing != options.texture_aliases->end()) {
            guid = existing->second;
        } else {
            options.texture_aliases->emplace(content_key, guid);
        }
    }
    return guid;
}

AssetGuid ResolveTexture(const assets::SidecarObject& object, std::string_view key,
                         const SidecarMergeOptions& options, std::string_view context,
                         std::vector<std::string>& warnings) {
    const std::optional<std::string> text = ReadString(object, key, context, warnings);
    if (!text || text->empty()) {
        return {};
    }
    return ResolveTexturePath(*text, options, std::string(context) + "." + std::string(key),
                              warnings);
}

template <typename T>
core::Result<void> AddComponent(SceneData& data, scene::EntityId owner, const T& value) {
    auto payload = scene::ComponentCodec<T>::Encode(value);
    if (!payload) {
        return std::move(payload).TakeError();
    }
    scene::ComponentChunk* chunk = data.FindComponent(scene::ComponentCodec<T>::type_id);
    if (chunk == nullptr) {
        scene::ComponentChunk created;
        created.type = scene::ComponentCodec<T>::type_id;
        created.schema_version = scene::ComponentCodec<T>::schema_version;
        data.components.push_back(std::move(created));
        chunk = &data.components.back();
    }
    chunk->owners.push_back(SceneEntityId{owner});
    chunk->payloads.push_back(std::move(payload).Value());
    return {};
}

template <typename T>
core::Result<void> AddSidecarEntity(SceneData& scene, std::uint64_t& next_entity_id,
                                    std::string name, Vec3 position, const T& component) {
    if (next_entity_id == 0) {
        next_entity_id = 1;
    }
    if (next_entity_id == std::numeric_limits<std::uint64_t>::max()) {
        return MakeError(ErrorCode::InvalidArgument, "scene entity id space is exhausted");
    }
    const scene::EntityId id = next_entity_id++;

    scene::SceneEntityRecord record;
    record.id = SceneEntityId{id};
    record.name = std::move(name);
    record.transform.position = position;
    scene.entities.push_back(std::move(record));

    scene::TransformComponent transform;
    transform.position = position;
    PARCEL_TRY(AddComponent(scene, id, transform));
    PARCEL_TRY(AddComponent(scene, id, component));
    return {};
}

float AbsFloor(float value, float minimum) { return std::max(std::abs(value), minimum); }

void SanitizeDecal(DecalComponent& decal) {
    decal.opacity = std::clamp(decal.opacity, 0.0f, 1.0f);
    decal.angle_fade = std::clamp(decal.angle_fade, 0.0f, 180.0f);
    decal.normal_blend = std::clamp(decal.normal_blend, 0.0f, 1.0f);
    decal.metallic = std::clamp(decal.metallic, 0.0f, 1.0f);
    decal.ambient_occlusion = std::clamp(decal.ambient_occlusion, 0.0f, 1.0f);
    decal.roughness = std::clamp(decal.roughness, 0.0f, 1.0f);
    decal.layer_mask = std::min<std::uint32_t>(decal.layer_mask, 0x7fU);
    decal.channel_mask = std::min<std::uint32_t>(decal.channel_mask, 0x1fU);
    decal.size = {AbsFloor(decal.size.x, 1.0e-3f), AbsFloor(decal.size.y, 1.0e-3f),
                  AbsFloor(decal.size.z, 1.0e-3f)};
}

void SanitizeProbe(ReflectionProbeComponent& probe) {
    probe.blend_distance = std::max(probe.blend_distance, 0.0f);
    probe.intensity = std::max(probe.intensity, 0.0f);
    probe.box_extents = {AbsFloor(probe.box_extents.x, 1.0e-3f),
                         AbsFloor(probe.box_extents.y, 1.0e-3f),
                         AbsFloor(probe.box_extents.z, 1.0e-3f)};
}

void SanitizeFog(LocalFogVolumeComponent& fog) {
    fog.size = {AbsFloor(fog.size.x, 1.0e-3f), AbsFloor(fog.size.y, 1.0e-3f),
                AbsFloor(fog.size.z, 1.0e-3f)};
    fog.albedo = {std::clamp(fog.albedo.x, 0.0f, 4.0f), std::clamp(fog.albedo.y, 0.0f, 4.0f),
                  std::clamp(fog.albedo.z, 0.0f, 4.0f)};
    fog.mean_free_path = std::clamp(fog.mean_free_path, 0.05f, 1000000.0f);
    fog.anisotropy = std::clamp(fog.anisotropy, -0.99f, 0.99f);
    fog.positive_fade = {std::max(fog.positive_fade.x, 0.0f), std::max(fog.positive_fade.y, 0.0f),
                         std::max(fog.positive_fade.z, 0.0f)};
    fog.negative_fade = {std::max(fog.negative_fade.x, 0.0f), std::max(fog.negative_fade.y, 0.0f),
                         std::max(fog.negative_fade.z, 0.0f)};
    fog.distance_fade_start = std::max(fog.distance_fade_start, 0.0f);
    fog.distance_fade_end = std::max(fog.distance_fade_end, fog.distance_fade_start);
    if (fog.distance_fade_end <= fog.distance_fade_start) {
        fog.distance_fade_end = fog.distance_fade_start + kMinFogDistanceFadeLength;
    }
    fog.mask_tiling = {std::max(std::abs(fog.mask_tiling.x), 0.001f),
                       std::max(std::abs(fog.mask_tiling.y), 0.001f),
                       std::max(std::abs(fog.mask_tiling.z), 0.001f)};
    fog.mask_intensity = std::clamp(fog.mask_intensity, 0.0f, 1.0f);
    if (fog.mask.IsZero()) {
        fog.mask_intensity = 0.0f;
    }
}

} // namespace

core::AssetGuid ResolveSidecarTexture(std::string_view value, const SidecarMergeOptions& options,
                                      std::string_view context,
                                      std::vector<std::string>& warnings) {
    return ResolveTexturePath(value, options, context, warnings);
}

core::AssetGuid ResolveSidecarIes(std::string_view value, const SidecarMergeOptions& options,
                                  std::string_view context, std::vector<std::string>& warnings) {
    const std::optional<std::string> relative =
        ResolveRelativeFile(value, options, context, warnings, "IES profile");
    return relative ? DefaultIesAssetGuid(*relative) : core::AssetGuid{};
}

core::Result<void> MergeSceneSidecar(SceneData& scene, const assets::SceneSidecar& sidecar,
                                     const SidecarMergeOptions& options,
                                     std::uint64_t& next_entity_id,
                                     std::vector<std::string>& warnings) {
    for (std::size_t index = 0; index < sidecar.decals.size(); ++index) {
        const assets::SidecarObject& object = sidecar.decals[index];
        const std::string context = ObjectContext("decals", index);
        DecalComponent decal;
        if (const auto value = ReadFloat(object, "opacity", context, warnings)) {
            decal.opacity = *value;
        }
        if (const auto value = ReadFloat(object, "angleFade", context, warnings)) {
            decal.angle_fade = *value;
        }
        if (const auto value = ReadFloat(object, "normalBlend", context, warnings)) {
            decal.normal_blend = *value;
        }
        if (const auto value = ReadFloat(object, "metallic", context, warnings)) {
            decal.metallic = *value;
        }
        if (const auto value = ReadFloat(object, "ao", context, warnings)) {
            decal.ambient_occlusion = *value;
        }
        if (const auto value = ReadFloat(object, "roughness", context, warnings)) {
            decal.roughness = *value;
        }
        if (const auto value = ReadUint(object, "layerMask", context, warnings)) {
            decal.layer_mask = *value;
        }
        if (const auto value = ReadUint(object, "channelMask", context, warnings)) {
            decal.channel_mask = *value;
        }
        if (const auto value = ReadBool(object, "enabled", context, warnings)) {
            decal.enabled = *value;
        }
        if (const auto value = ReadVec3(object, "position", context, warnings)) {
            decal.position = *value;
        }
        if (const auto value = ReadVec3(object, "rotation", context, warnings)) {
            decal.rotation = *value;
        }
        if (const auto value = ReadVec3(object, "size", context, warnings)) {
            decal.size = *value;
        }
        if (const auto value = ReadVec4(object, "baseColor", context, warnings)) {
            decal.base_color = *value;
        }
        decal.albedo = ResolveTexture(object, "texture", options, context, warnings);
        decal.normal = ResolveTexture(object, "normalTexture", options, context, warnings);
        decal.mask = ResolveTexture(object, "maskTexture", options, context, warnings);
        SanitizeDecal(decal);
        const std::string name =
            object.name.empty() ? "Decal " + std::to_string(index) : object.name;
        PARCEL_TRY(AddSidecarEntity(scene, next_entity_id, name, decal.position, decal));
    }

    for (std::size_t index = 0; index < sidecar.reflection_probes.size(); ++index) {
        const assets::SidecarObject& object = sidecar.reflection_probes[index];
        const std::string context = ObjectContext("reflectionProbes", index);
        ReflectionProbeComponent probe;
        if (const auto value = ReadFloat(object, "blendDistance", context, warnings)) {
            probe.blend_distance = *value;
        }
        if (const auto value = ReadFloat(object, "intensity", context, warnings)) {
            probe.intensity = *value;
        }
        if (const auto value = ReadBool(object, "enabled", context, warnings)) {
            probe.enabled = *value;
        }
        if (const auto value = ReadVec3(object, "position", context, warnings)) {
            probe.position = *value;
        }
        if (const auto value = ReadVec3(object, "rotation", context, warnings)) {
            probe.rotation = *value;
        }
        if (const auto value = ReadVec3(object, "boxExtents", context, warnings)) {
            probe.box_extents = *value;
        }
        probe.image = ResolveTexture(object, "texture", options, context, warnings);
        if (probe.image.IsZero()) {
            Warn(warnings, context + ": reflection probe requires a valid texture");
            continue;
        }
        SanitizeProbe(probe);
        const std::string name =
            object.name.empty() ? "Probe " + std::to_string(index) : object.name;
        PARCEL_TRY(AddSidecarEntity(scene, next_entity_id, name, probe.position, probe));
    }

    for (std::size_t index = 0; index < sidecar.local_fog_volumes.size(); ++index) {
        const assets::SidecarObject& object = sidecar.local_fog_volumes[index];
        const std::string context = ObjectContext("localFogVolumes", index);
        LocalFogVolumeComponent fog;
        if (const auto value = ReadBool(object, "enabled", context, warnings)) {
            fog.enabled = *value;
        }
        if (const auto value = ReadVec3(object, "position", context, warnings)) {
            fog.center = *value;
        }
        if (const auto value = ReadVec3(object, "rotation", context, warnings)) {
            fog.rotation = *value;
        }
        if (const auto value = ReadVec3(object, "size", context, warnings)) {
            fog.size = *value;
        }
        if (const auto value = ReadVec3(object, "albedo", context, warnings)) {
            fog.albedo = *value;
        }
        if (const auto value = ReadVec3(object, "positiveFade", context, warnings)) {
            fog.positive_fade = *value;
        }
        if (const auto value = ReadVec3(object, "negativeFade", context, warnings)) {
            fog.negative_fade = *value;
        }
        if (const auto value = ReadVec3(object, "maskTiling", context, warnings)) {
            fog.mask_tiling = *value;
        }
        if (const auto value = ReadVec3(object, "maskScroll", context, warnings)) {
            fog.mask_scroll = *value;
        }
        if (const auto value = ReadFloat(object, "meanFreePath", context, warnings)) {
            fog.mean_free_path = *value;
        }
        if (const auto value = ReadFloat(object, "anisotropy", context, warnings)) {
            fog.anisotropy = *value;
        }
        if (const auto value = ReadFloat(object, "distanceFadeStart", context, warnings)) {
            fog.distance_fade_start = *value;
        }
        if (const auto value = ReadFloat(object, "distanceFadeEnd", context, warnings)) {
            fog.distance_fade_end = *value;
        }
        if (const auto value = ReadFloat(object, "maskIntensity", context, warnings)) {
            fog.mask_intensity = *value;
        }
        if (const auto value = ReadBool(object, "invertFade", context, warnings)) {
            fog.invert_fade = *value;
        }
        if (const auto value = ReadBool(object, "maskInvert", context, warnings)) {
            fog.mask_invert = *value;
        }
        if (const auto value = ReadString(object, "shape", context, warnings)) {
            const std::string shape = LowerAscii(*value);
            if (shape == "box" || shape == "obb") {
                fog.shape = LocalFogShape::Box;
            } else if (shape == "sphere" || shape == "ellipsoid") {
                fog.shape = LocalFogShape::Sphere;
            } else {
                Warn(warnings, context + ".shape: expected box or sphere");
            }
        }
        if (const auto value = ReadString(object, "falloffMode", context, warnings)) {
            const std::string falloff = LowerAscii(*value);
            if (falloff == "linear") {
                fog.falloff = LocalFogFalloff::Linear;
            } else if (falloff == "exponential") {
                fog.falloff = LocalFogFalloff::Exponential;
            } else {
                Warn(warnings, context + ".falloffMode: expected linear or exponential");
            }
        }
        if (const auto value = ReadString(object, "maskPlane", context, warnings)) {
            const std::string plane = LowerAscii(*value);
            if (plane == "xy") {
                fog.mask_plane = LocalFogMaskPlane::XY;
            } else if (plane == "xz") {
                fog.mask_plane = LocalFogMaskPlane::XZ;
            } else if (plane == "yz") {
                fog.mask_plane = LocalFogMaskPlane::YZ;
            } else {
                Warn(warnings, context + ".maskPlane: expected xy, xz or yz");
            }
        }
        fog.mask = ResolveTexture(object, "maskTexture", options, context, warnings);
        SanitizeFog(fog);
        const std::string name =
            object.name.empty() ? "Local Fog " + std::to_string(index) : object.name;
        PARCEL_TRY(AddSidecarEntity(scene, next_entity_id, name, fog.center, fog));
    }
    return {};
}

} // namespace parcel::import
