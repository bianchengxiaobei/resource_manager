#include "parcel/import/content_hash.hpp"
#include "parcel/import/importer.hpp"
#include "parcel/import/mesh_optimizer.hpp"
#include "parcel/import/meta.hpp"
#include "parcel/import/sidecar_merge.hpp"
#include "parcel/import/texture_dedup.hpp"
#include "texture_decode.hpp"
#include "texture_transform.hpp"

#include "parcel/assets/animation_clip_asset.hpp"
#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/mesh_asset.hpp"
#include "parcel/assets/scene_asset.hpp"
#include "parcel/assets/sidecar.hpp"
#include "parcel/core/error.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/path.hpp"
#include "parcel/scene/component_codec.hpp"
#include "parcel/scene/scene_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <meshoptimizer.h>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

namespace parcel::import {
namespace {

using core::AssetGuid;
using core::ErrorCode;
using core::MakeError;
using scene::ComponentCodec;
using scene::SceneData;
using scene::SceneEntityId;

constexpr std::uint64_t kMaxVertices = 10ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t kMaxIndices = 10ULL * 1000ULL * 1000ULL;
constexpr std::uint32_t kMaxPrimitiveTargets = 256;
constexpr std::uint64_t kMaxBufferBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxEmbeddedImageBytes = 64ULL * 1024ULL * 1024ULL;

std::filesystem::path Utf8Path(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

std::string NameOr(const char* name, std::string fallback) {
    if (name == nullptr || name[0] == '\0') {
        return fallback;
    }
    return std::string(name);
}

bool Finite(float value) { return std::isfinite(value); }

bool Finite(const Vec3& value) { return Finite(value.x) && Finite(value.y) && Finite(value.z); }

bool Finite(const Vec4& value) {
    return Finite(value.x) && Finite(value.y) && Finite(value.z) && Finite(value.w);
}

Vec3 ToVec3(const cgltf_float* value) { return Vec3{value[0], value[1], value[2]}; }

Vec4 ToVec4(const cgltf_float* value) { return Vec4{value[0], value[1], value[2], value[3]}; }

Mat4 ToMat4(const cgltf_float* value) {
    Mat4 result;
    for (std::size_t index = 0; index < 16; ++index) {
        result.m[index] = value[index];
    }
    return result;
}

float Length(const Vec3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vec3 Normalize(const Vec3& value) {
    const float length = Length(value);
    if (length <= 1.0e-8f) {
        return {0.0f, 0.0f, 1.0f};
    }
    return {value.x / length, value.y / length, value.z / length};
}

Quat Normalize(const Quat& value) {
    const float length =
        std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
    if (length <= 1.0e-8f) {
        return Quat::Identity();
    }
    return Quat{value.x / length, value.y / length, value.z / length, value.w / length};
}

scene::TransformData DecomposeTransform(const Mat4& matrix, float scale) {
    scene::TransformData result;
    const float sx = Length({matrix.m[0], matrix.m[1], matrix.m[2]});
    const float sy = Length({matrix.m[4], matrix.m[5], matrix.m[6]});
    const float sz = Length({matrix.m[8], matrix.m[9], matrix.m[10]});
    const float determinant =
        matrix.m[0] * (matrix.m[5] * matrix.m[10] - matrix.m[6] * matrix.m[9]) -
        matrix.m[4] * (matrix.m[1] * matrix.m[10] - matrix.m[2] * matrix.m[9]) +
        matrix.m[8] * (matrix.m[1] * matrix.m[6] - matrix.m[2] * matrix.m[5]);

    const float safe_sx = sx > 1.0e-8f ? (determinant < 0.0f ? -sx : sx) : 1.0f;
    const float safe_sy = sy > 1.0e-8f ? sy : 1.0f;
    const float safe_sz = sz > 1.0e-8f ? sz : 1.0f;
    const float r00 = matrix.m[0] / safe_sx;
    const float r10 = matrix.m[1] / safe_sx;
    const float r20 = matrix.m[2] / safe_sx;
    const float r01 = matrix.m[4] / safe_sy;
    const float r11 = matrix.m[5] / safe_sy;
    const float r21 = matrix.m[6] / safe_sy;
    const float r02 = matrix.m[8] / safe_sz;
    const float r12 = matrix.m[9] / safe_sz;
    const float r22 = matrix.m[10] / safe_sz;

    const float trace = r00 + r11 + r22;
    Quat rotation;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        rotation = Quat{(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        rotation = Quat{0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        rotation = Quat{(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        rotation = Quat{(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
    }

    result.position = {matrix.m[12] * scale, matrix.m[13] * scale, matrix.m[14] * scale};
    result.rotation = Normalize(rotation);
    result.scale = {safe_sx * scale, safe_sy * scale, safe_sz * scale};
    if (!Finite(result.position) || !Finite(result.scale)) {
        result = {};
    }
    return result;
}

scene::TransformData NodeTransform(const cgltf_node& node, float scale) {
    if (!node.has_matrix) {
        scene::TransformData transform;
        transform.position = {node.translation[0] * scale, node.translation[1] * scale,
                              node.translation[2] * scale};
        transform.rotation =
            Normalize(Quat{node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]});
        transform.scale = {node.scale[0] * scale, node.scale[1] * scale, node.scale[2] * scale};
        if (!Finite(transform.position) || !Finite(transform.scale)) {
            return {};
        }
        return transform;
    }
    cgltf_float matrix[16];
    cgltf_node_transform_local(&node, matrix);
    return DecomposeTransform(ToMat4(matrix), scale);
}

const cgltf_accessor* FindAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type,
                                    int index = 0) {
    for (cgltf_size attribute_index = 0; attribute_index < primitive.attributes_count;
         ++attribute_index) {
        const cgltf_attribute& attribute = primitive.attributes[attribute_index];
        if (attribute.type == type && attribute.index == index) {
            return attribute.data;
        }
    }
    return nullptr;
}

const cgltf_accessor* FindMorphAttribute(const cgltf_morph_target& target,
                                         cgltf_attribute_type type) {
    for (cgltf_size attribute_index = 0; attribute_index < target.attributes_count;
         ++attribute_index) {
        const cgltf_attribute& attribute = target.attributes[attribute_index];
        if (attribute.type == type) {
            return attribute.data;
        }
    }
    return nullptr;
}

bool UnpackFloats(const cgltf_accessor* accessor, std::size_t components, std::vector<float>& data,
                  std::size_t& count) {
    data.clear();
    count = 0;
    if (accessor == nullptr || accessor->count == 0) {
        return false;
    }
    const std::size_t accessor_components = cgltf_num_components(accessor->type);
    if (accessor_components < components) {
        return false;
    }
    if (accessor->count > std::numeric_limits<std::size_t>::max() / accessor_components) {
        return false;
    }
    const std::size_t required = static_cast<std::size_t>(accessor->count) * accessor_components;
    data.resize(required);
    const cgltf_size written = cgltf_accessor_unpack_floats(accessor, data.data(), required);
    if (written < required) {
        data.clear();
        return false;
    }
    count = static_cast<std::size_t>(accessor->count);
    return true;
}

bool UnpackIndices(const cgltf_accessor* accessor, std::vector<std::uint32_t>& indices) {
    indices.clear();
    if (accessor == nullptr || accessor->count == 0) {
        return false;
    }
    indices.resize(static_cast<std::size_t>(accessor->count));
    const cgltf_size written = cgltf_accessor_unpack_indices(
        accessor, indices.data(), sizeof(std::uint32_t), accessor->count);
    if (written != accessor->count) {
        indices.clear();
        return false;
    }
    return true;
}

std::string LowerExtension(std::string_view path) {
    std::string extension(core::path::Extension(path));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

std::string PercentDecode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%' || index + 2 >= value.size()) {
            result.push_back(value[index]);
            continue;
        }
        const auto digit = [](char character) -> int {
            if (character >= '0' && character <= '9') {
                return character - '0';
            }
            if (character >= 'a' && character <= 'f') {
                return character - 'a' + 10;
            }
            if (character >= 'A' && character <= 'F') {
                return character - 'A' + 10;
            }
            return -1;
        };
        const int high = digit(value[index + 1]);
        const int low = digit(value[index + 2]);
        if (high < 0 || low < 0) {
            result.push_back(value[index]);
            continue;
        }
        result.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return result;
}

bool DecodeBase64(std::string_view text, std::vector<std::byte>& output, std::string& error) {
    output.clear();
    if (text.size() > kMaxEmbeddedImageBytes * 4ULL / 3ULL + 4ULL) {
        error = "base64 image is too large";
        return false;
    }
    output.reserve(text.size() * 3ULL / 4ULL);
    std::uint32_t accumulator = 0;
    int bits = 0;
    int padding = 0;
    for (const char character : text) {
        if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
            continue;
        }
        if (padding != 0 && character != '=') {
            error = "base64 image has data after padding";
            return false;
        }
        if (character == '=') {
            ++padding;
            if (padding > 2) {
                error = "base64 image has invalid padding";
                return false;
            }
            continue;
        }
        int value = -1;
        if (character >= 'A' && character <= 'Z') {
            value = character - 'A';
        } else if (character >= 'a' && character <= 'z') {
            value = character - 'a' + 26;
        } else if (character >= '0' && character <= '9') {
            value = character - '0' + 52;
        } else if (character == '+') {
            value = 62;
        } else if (character == '/') {
            value = 63;
        }
        if (value < 0) {
            error = "base64 image has an invalid character";
            return false;
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (output.size() >= kMaxEmbeddedImageBytes) {
                error = "embedded image is too large";
                return false;
            }
            output.push_back(static_cast<std::byte>((accumulator >> bits) & 0xffU));
        }
    }
    return true;
}

bool ReadEmbeddedImage(const cgltf_image* image, std::vector<std::byte>& bytes,
                       std::string& error) {
    if (image == nullptr) {
        error = "image is missing";
        return false;
    }
    if (image->buffer_view != nullptr) {
        const cgltf_buffer_view& view = *image->buffer_view;
        if (view.size == 0 || view.size > kMaxEmbeddedImageBytes) {
            error = "image buffer view has an invalid size";
            return false;
        }
        if (view.data == nullptr && (view.buffer == nullptr || view.buffer->data == nullptr)) {
            error = "image buffer view has no data";
            return false;
        }
        const auto* begin = reinterpret_cast<const std::byte*>(cgltf_buffer_view_data(&view));
        if (begin == nullptr) {
            error = "image buffer view has no data";
            return false;
        }
        bytes.assign(begin, begin + view.size);
        return true;
    }
    if (image->uri == nullptr) {
        error = "image has no URI or buffer view";
        return false;
    }
    const std::string_view uri(image->uri);
    if (!uri.starts_with("data:")) {
        error = "image is not a data URI";
        return false;
    }
    const auto comma = uri.find(',');
    if (comma == std::string_view::npos) {
        error = "data URI has no payload";
        return false;
    }
    const std::string_view header = uri.substr(0, comma);
    if (header.find(";base64") == std::string_view::npos) {
        error = "data URI image is not base64 encoded";
        return false;
    }
    return DecodeBase64(uri.substr(comma + 1), bytes, error);
}

core::Result<std::string> ProjectRelative(std::string_view root, std::string_view path) {
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(Utf8Path(path), Utf8Path(root), error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to resolve project path: " + error.message())
            .WithPath(std::string(path));
    }
    const std::u8string text = relative.u8string();
    std::string result =
        core::path::Normalize(std::string(reinterpret_cast<const char*>(text.data()), text.size()));
    if (result.rfind("../", 0) == 0 || result == "..") {
        return MakeError(ErrorCode::InvalidArgument, "path is outside the project");
    }
    return result;
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

struct ResolvedExternalPath {
    std::string absolute;
    std::string relative;
};

core::Result<ResolvedExternalPath> ResolveGlTFExternalPath(std::string_view project_root,
                                                           std::string_view source_directory,
                                                           std::string_view uri,
                                                           std::string_view kind) {
    const std::string decoded = PercentDecode(uri);
    const std::string normalized = core::path::Normalize(decoded);
    if (normalized.empty()) {
        return MakeError(ErrorCode::InvalidArgument,
                         "glTF " + std::string(kind) + " path is empty");
    }

    std::string absolute;
    if (core::path::IsAbsolute(normalized)) {
        absolute = normalized;
    } else {
        absolute = std::string(source_directory);
        if (!absolute.empty() && absolute.back() != '/') {
            absolute.push_back('/');
        }
        absolute += normalized;
        absolute = core::path::Normalize(absolute);
    }

    auto relative = ProjectRelative(project_root, absolute);
    if (!relative) {
        return MakeError(ErrorCode::InvalidArgument,
                         "glTF " + std::string(kind) + " path is outside the project: " + absolute)
            .WithPath(std::string(uri));
    }
    return ResolvedExternalPath{std::move(absolute), std::move(relative).Value()};
}

std::string FindSidecarPath(std::string_view source_path) {
    std::error_code error;
    const std::string direct = std::string(source_path) + ".parcel.json";
    if (std::filesystem::is_regular_file(Utf8Path(direct), error)) {
        return direct;
    }
    const std::string normalized = core::path::Normalize(source_path);
    const auto dot = normalized.find_last_of('.');
    const auto slash = normalized.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        const std::string alternate = normalized.substr(0, dot) + ".parcel.json";
        if (std::filesystem::is_regular_file(Utf8Path(alternate), error)) {
            return alternate;
        }
    }
    return {};
}

AssetGuid ResolveProjectReference(std::string_view value, const ImportRequest& request,
                                  std::vector<std::string>& warnings, std::string_view context) {
    if (value.empty()) {
        return {};
    }
    if (!core::path::IsSafeRelative(value)) {
        warnings.push_back(std::string(context) + " is not a safe relative path");
        return {};
    }
    const std::string relative = core::path::Normalize(value);
    auto resolved = core::path::ResolveWithin(request.project_root, relative);
    if (!resolved) {
        warnings.push_back(std::string(context) + " is outside the project");
        return {};
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(Utf8Path(resolved.Value()), error)) {
        warnings.push_back(std::string(context) + " is missing: " + resolved.Value());
        return {};
    }
    return MakeSourceGuid(relative);
}

std::optional<scene::SceneEntityRecord*> FindEntityByName(SceneData& scene, std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (scene::SceneEntityRecord& entity : scene.entities) {
        if (entity.name == name) {
            return &entity;
        }
    }
    return std::nullopt;
}

template <typename T>
void SetComponent(SceneData& scene, scene::EntityId owner, const T& component);

core::Result<void> ApplySidecar(const ImportRequest& request, assets::SceneAsset& scene_asset,
                                std::unordered_map<std::string, AssetGuid>& textures_by_hash,
                                std::vector<std::string>& warnings) {
    const std::string sidecar_path = FindSidecarPath(request.source_path);
    if (sidecar_path.empty()) {
        return {};
    }
    auto sidecar = assets::LoadSceneSidecarFile(sidecar_path);
    if (!sidecar) {
        return std::move(sidecar).TakeError();
    }
    SceneData& scene = scene_asset.scene;
    scene.pipeline_settings = ResolveProjectReference(sidecar.Value().settings.pipeline, request,
                                                      warnings, "sidecar pipeline");
    scene.frame_settings =
        ResolveProjectReference(sidecar.Value().settings.frame, request, warnings, "sidecar frame");
    scene.post_process_profile = ResolveProjectReference(sidecar.Value().settings.post_process,
                                                         request, warnings, "sidecar postProcess");
    scene.shadow_settings = ResolveProjectReference(sidecar.Value().settings.shadows, request,
                                                    warnings, "sidecar shadows");
    scene.volumetric_fog_profile = ResolveProjectReference(
        sidecar.Value().settings.volumetric_fog, request, warnings, "sidecar volumetricFog");

    for (const assets::SidecarCamera& sidecar_camera : sidecar.Value().cameras) {
        const auto entity = FindEntityByName(scene, sidecar_camera.name);
        if (!entity.has_value()) {
            warnings.push_back("sidecar camera has no matching node: " + sidecar_camera.name);
            continue;
        }
        scene::CameraComponent component;
        component.projection = sidecar_camera.projection == assets::SidecarProjection::Orthographic
                                   ? scene::CameraComponent::Projection::Orthographic
                                   : scene::CameraComponent::Projection::Perspective;
        component.yfov = sidecar_camera.yfov;
        component.aspect = sidecar_camera.aspect;
        component.near_clip = sidecar_camera.near_clip;
        component.far_clip = sidecar_camera.far_clip;
        component.xmag = sidecar_camera.xmag;
        component.ymag = sidecar_camera.ymag;
        component.clear_color = sidecar_camera.clear_color;
        component.culling_mask = sidecar_camera.culling_mask;
        component.priority = sidecar_camera.priority;
        SetComponent(scene, entity.value()->id.value, component);
    }

    SidecarMergeOptions merge_options;
    merge_options.project_root = request.project_root;
    merge_options.source_path = request.source_path;
    merge_options.texture_aliases = &textures_by_hash;
    merge_options.canonical_textures = request.texture_index;

    for (const assets::SidecarObject& object : sidecar.Value().lights) {
        const auto entity = FindEntityByName(scene, object.name);
        if (!entity.has_value()) {
            warnings.push_back("sidecar light has no matching node: " + object.name);
            continue;
        }
        scene::LightComponent component;
        if (const auto* type = object.data.Find("type"); type != nullptr && type->IsString()) {
            if (type->string_value == "directional") {
                component.type = scene::LightType::Directional;
            } else if (type->string_value == "spot") {
                component.type = scene::LightType::Spot;
            } else if (type->string_value == "area") {
                component.type = scene::LightType::Area;
            }
        }
        if (const auto* color = object.data.Find("color");
            color != nullptr && color->IsArray() && color->array_value.size() >= 3) {
            component.color = {static_cast<float>(color->array_value[0].number_value),
                               static_cast<float>(color->array_value[1].number_value),
                               static_cast<float>(color->array_value[2].number_value)};
        }
        if (const auto* intensity = object.data.Find("intensity");
            intensity != nullptr && intensity->IsNumber()) {
            component.intensity = static_cast<float>(intensity->number_value);
        }
        if (const auto* range = object.data.Find("range"); range != nullptr && range->IsNumber()) {
            component.range = static_cast<float>(range->number_value);
        }
        if (const auto* shadows = object.data.Find("castShadows");
            shadows != nullptr && shadows->IsBool()) {
            component.cast_shadows = shadows->bool_value;
        }
        if (const auto* cookie = object.data.Find("cookie");
            cookie != nullptr && cookie->IsString()) {
            component.cookie =
                ResolveSidecarTexture(cookie->string_value, merge_options,
                                      "sidecar light '" + object.name + "' cookie", warnings);
            if (!component.cookie.IsZero()) {
                component.cookie_mode = scene::LightCookieMode::Clamp;
                component.cookie_type = component.type == scene::LightType::Point
                                            ? scene::LightCookieType::PointOctahedral
                                            : scene::LightCookieType::Spot2D;
            }
        }
        if (const auto* mode = object.data.Find("cookieMode");
            mode != nullptr && mode->IsString()) {
            if (mode->string_value == "none") {
                component.cookie_mode = scene::LightCookieMode::None;
            } else if (mode->string_value == "clamp") {
                component.cookie_mode = scene::LightCookieMode::Clamp;
            } else if (mode->string_value == "repeat") {
                component.cookie_mode = scene::LightCookieMode::Repeat;
            }
        }
        if (const auto* cookie_type = object.data.Find("cookieType");
            cookie_type != nullptr && cookie_type->IsString()) {
            if (cookie_type->string_value == "none") {
                component.cookie_type = scene::LightCookieType::None;
            } else if (cookie_type->string_value == "spot2d" ||
                       cookie_type->string_value == "spot") {
                component.cookie_type = scene::LightCookieType::Spot2D;
            } else if (cookie_type->string_value == "pointoctahedral" ||
                       cookie_type->string_value == "point") {
                component.cookie_type = scene::LightCookieType::PointOctahedral;
            }
        }
        if (const auto* ies = object.data.Find("ies"); ies != nullptr && ies->IsString()) {
            component.ies = ResolveSidecarIes(ies->string_value, merge_options,
                                              "sidecar light '" + object.name + "' ies", warnings);
            component.has_ies = !component.ies.IsZero();
        }
        if (const auto* cookie_scale = object.data.Find("cookieScale");
            cookie_scale != nullptr && cookie_scale->IsArray() &&
            cookie_scale->array_value.size() >= 2) {
            component.cookie_scale = {
                static_cast<float>(cookie_scale->array_value[0].number_value),
                static_cast<float>(cookie_scale->array_value[1].number_value)};
        }
        if (const auto* offset = object.data.Find("cookieOffset");
            offset != nullptr && offset->IsArray() && offset->array_value.size() >= 2) {
            component.cookie_offset = {static_cast<float>(offset->array_value[0].number_value),
                                       static_cast<float>(offset->array_value[1].number_value)};
        }
        SetComponent(scene, entity.value()->id.value, component);
    }

    std::uint64_t next_entity_id = scene.entities.size() + 1;
    return MergeSceneSidecar(scene, sidecar.Value(), merge_options, next_entity_id, warnings);
}

core::Result<void> ValidateExternalBuffers(const cgltf_data& data, const ImportRequest& request) {
    const std::string source_directory = DirectoryOf(request.source_path);
    for (cgltf_size index = 0; index < data.buffers_count; ++index) {
        const cgltf_buffer& buffer = data.buffers[index];
        if (buffer.size > kMaxBufferBytes) {
            return MakeError(ErrorCode::InvalidArgument, "glTF buffer is too large")
                .WithPath(request.source_path);
        }
        if (buffer.uri == nullptr || std::string_view(buffer.uri).starts_with("data:")) {
            continue;
        }
        auto resolved =
            ResolveGlTFExternalPath(request.project_root, source_directory, buffer.uri, "buffer");
        if (!resolved) {
            auto error = std::move(resolved).TakeError();
            error.WithPath(request.source_path);
            return error;
        }
    }
    return {};
}

core::Result<void> DecodeMeshoptBufferViews(cgltf_data& data, const ImportRequest& request) {
    std::uint64_t total_decoded = 0;
    for (cgltf_size view_index = 0; view_index < data.buffer_views_count; ++view_index) {
        cgltf_buffer_view& view = data.buffer_views[view_index];
        if (!view.has_meshopt_compression) {
            continue;
        }
        const cgltf_meshopt_compression& compressed = view.meshopt_compression;
        if (view.data != nullptr) {
            return MakeError(ErrorCode::Corrupt,
                             "glTF meshopt buffer view already has decoded data")
                .WithPath(request.source_path);
        }
        if (compressed.buffer == nullptr || compressed.buffer->data == nullptr) {
            return MakeError(ErrorCode::Corrupt, "glTF meshopt source buffer has no data")
                .WithPath(request.source_path);
        }
        if (compressed.stride == 0 || compressed.count == 0) {
            return MakeError(ErrorCode::Corrupt,
                             "glTF meshopt buffer view has an invalid stride or count")
                .WithPath(request.source_path);
        }
        if (compressed.stride > kMaxBufferBytes ||
            compressed.count > kMaxBufferBytes / compressed.stride) {
            return MakeError(ErrorCode::InvalidArgument, "glTF meshopt buffer view is too large")
                .WithPath(request.source_path);
        }
        const std::uint64_t decoded_size =
            static_cast<std::uint64_t>(compressed.stride) * compressed.count;
        if (decoded_size > kMaxBufferBytes || total_decoded > kMaxBufferBytes - decoded_size) {
            return MakeError(ErrorCode::InvalidArgument,
                             "glTF meshopt data exceeds the decode budget")
                .WithPath(request.source_path);
        }
        if (compressed.offset > compressed.buffer->size ||
            compressed.size > compressed.buffer->size - compressed.offset) {
            return MakeError(ErrorCode::Corrupt, "glTF meshopt source range is outside the buffer")
                .WithPath(request.source_path);
        }

        void* decoded =
            data.memory.alloc_func(data.memory.user_data, static_cast<cgltf_size>(decoded_size));
        if (decoded == nullptr) {
            return MakeError(ErrorCode::Internal, "failed to allocate glTF meshopt buffer")
                .WithPath(request.source_path);
        }
        const auto* source =
            static_cast<const unsigned char*>(compressed.buffer->data) + compressed.offset;
        int decode_result = -1;
        switch (compressed.mode) {
        case cgltf_meshopt_compression_mode_attributes:
            decode_result = meshopt_decodeVertexBuffer(decoded, compressed.count, compressed.stride,
                                                       source, compressed.size);
            break;
        case cgltf_meshopt_compression_mode_triangles:
            decode_result = meshopt_decodeIndexBuffer(decoded, compressed.count, compressed.stride,
                                                      source, compressed.size);
            break;
        case cgltf_meshopt_compression_mode_indices:
            decode_result = meshopt_decodeIndexSequence(decoded, compressed.count,
                                                        compressed.stride, source, compressed.size);
            break;
        default:
            break;
        }
        if (decode_result != 0) {
            data.memory.free_func(data.memory.user_data, decoded);
            return MakeError(ErrorCode::Corrupt, "failed to decode glTF meshopt buffer view")
                .WithPath(request.source_path);
        }
        if (compressed.mode == cgltf_meshopt_compression_mode_attributes) {
            switch (compressed.filter) {
            case cgltf_meshopt_compression_filter_octahedral:
                meshopt_decodeFilterOct(decoded, compressed.count, compressed.stride);
                break;
            case cgltf_meshopt_compression_filter_quaternion:
                meshopt_decodeFilterQuat(decoded, compressed.count, compressed.stride);
                break;
            case cgltf_meshopt_compression_filter_exponential:
                meshopt_decodeFilterExp(decoded, compressed.count, compressed.stride);
                break;
            default:
                break;
            }
        }

        view.data = decoded;
        if (view.stride == 0) {
            view.stride = compressed.stride;
        }
        for (cgltf_size accessor_index = 0; accessor_index < data.accessors_count;
             ++accessor_index) {
            cgltf_accessor& accessor = data.accessors[accessor_index];
            if (accessor.buffer_view == &view) {
                accessor.stride = compressed.stride;
            }
        }
        total_decoded += decoded_size;
    }
    return {};
}

struct TextureResolver {
    const ImportRequest& request;
    std::vector<std::string>& warnings;
    std::vector<AssetGuid>& dependencies;
    std::unordered_map<const cgltf_image*, AssetGuid> cache;
    std::unordered_map<std::string, AssetGuid> baked_cache;
    std::unordered_map<std::string, AssetGuid>& by_content;
    std::vector<CookedAsset>& assets;
    const cgltf_data* data = nullptr;

    AssetGuid ResolveEmbedded(const cgltf_image* image) {
        std::vector<std::byte> bytes;
        std::string decode_error;
        if (!ReadEmbeddedImage(image, bytes, decode_error)) {
            warnings.push_back("glTF embedded image is invalid: " +
                               NameOr(image == nullptr ? nullptr : image->name, "image") + ": " +
                               decode_error);
            cache.emplace(image, AssetGuid{});
            return {};
        }

        std::size_t image_index = 0;
        if (data != nullptr && data->images != nullptr && image >= data->images &&
            image < data->images + data->images_count) {
            image_index = static_cast<std::size_t>(image - data->images);
        }
        const std::string target =
            request.settings != nullptr ? SettingString(*request.settings, "target", "") : "";
        const assets::JsonValue texture_settings = MakeDefaultImportSettings("texture", target);
        auto decoded = DecodeTextureAsset(
            bytes, texture_settings, request.source_path + ":image:" + std::to_string(image_index));
        if (!decoded) {
            warnings.push_back("failed to decode glTF embedded image '" +
                               NameOr(image->name, "image") +
                               "': " + decoded.GetError().ToString());
            cache.emplace(image, AssetGuid{});
            return {};
        }

        const AssetGuid guid = MakeSubAssetGuid(request.source_guid, "texture",
                                                "gltf-image:" + std::to_string(image_index));
        CookedAsset cooked;
        cooked.guid = guid;
        cooked.local_id = "texture:gltf:" + std::to_string(image_index);
        cooked.role = "texture";
        cooked.object = std::move(decoded).Value();
        assets.push_back(std::move(cooked));

        if (std::find(dependencies.begin(), dependencies.end(), guid) == dependencies.end()) {
            dependencies.push_back(guid);
        }
        cache.emplace(image, guid);
        return guid;
    }

    AssetGuid Resolve(const cgltf_image* image) {
        if (image == nullptr) {
            return {};
        }
        if (const auto cached = cache.find(image); cached != cache.end()) {
            return cached->second;
        }
        if (image->buffer_view != nullptr ||
            (image->uri != nullptr && std::string_view(image->uri).starts_with("data:"))) {
            return ResolveEmbedded(image);
        }
        if (image->uri == nullptr) {
            warnings.push_back("glTF image has no URI or buffer view: " +
                               NameOr(image->name, "image"));
            cache.emplace(image, AssetGuid{});
            return {};
        }

        const std::string source_directory = DirectoryOf(request.source_path);
        auto resolved =
            ResolveGlTFExternalPath(request.project_root, source_directory, image->uri, "image");
        if (!resolved) {
            warnings.push_back(resolved.GetError().message);
            cache.emplace(image, AssetGuid{});
            return {};
        }
        const ResolvedExternalPath& path = resolved.Value();
        std::error_code error;
        if (!std::filesystem::is_regular_file(Utf8Path(path.absolute), error)) {
            warnings.push_back("missing glTF image: " + path.absolute);
            cache.emplace(image, AssetGuid{});
            return {};
        }

        AssetGuid guid;
        if (const TextureCanonicalAsset* canonical =
                FindCanonicalTextureByPath(request.texture_index, path.relative);
            canonical != nullptr) {
            guid = canonical->asset_guid;
        } else {
            guid = DefaultTextureAssetGuid(path.relative);
        }
        if (const auto hash = core::Sha256File(path.absolute); hash) {
            by_content.emplace(hash.Value().ToHex(), guid);
        }
        if (!guid.IsZero() &&
            std::find(dependencies.begin(), dependencies.end(), guid) == dependencies.end()) {
            dependencies.push_back(guid);
        }
        cache.emplace(image, guid);
        return guid;
    }

    [[nodiscard]] std::size_t ImageIndex(const cgltf_image* image) const noexcept {
        if (data != nullptr && data->images != nullptr && image >= data->images &&
            image < data->images + data->images_count) {
            return static_cast<std::size_t>(image - data->images);
        }
        return 0;
    }

    [[nodiscard]] bool ReadImageBytes(const cgltf_image* image, std::vector<std::byte>& bytes,
                                      std::string& error) const {
        if (image == nullptr) {
            error = "image is missing";
            return false;
        }
        if (image->buffer_view != nullptr ||
            (image->uri != nullptr && std::string_view(image->uri).starts_with("data:"))) {
            return ReadEmbeddedImage(image, bytes, error);
        }
        if (image->uri == nullptr) {
            error = "image has no URI or buffer view";
            return false;
        }

        auto resolved = ResolveGlTFExternalPath(
            request.project_root, DirectoryOf(request.source_path), image->uri, "image");
        if (!resolved) {
            error = resolved.GetError().message;
            return false;
        }
        std::error_code file_error;
        if (!std::filesystem::is_regular_file(Utf8Path(resolved.Value().absolute), file_error)) {
            error = "missing image: " + resolved.Value().absolute;
            return false;
        }
        std::ifstream file(Utf8Path(resolved.Value().absolute), std::ios::binary | std::ios::ate);
        if (!file) {
            error = "failed to open image: " + resolved.Value().absolute;
            return false;
        }
        const std::streamoff length = file.tellg();
        if (length < 0 || static_cast<std::uint64_t>(length) > kMaxEmbeddedImageBytes) {
            error = "image has an invalid size";
            return false;
        }
        bytes.resize(static_cast<std::size_t>(length));
        file.seekg(0);
        if (!bytes.empty()) {
            file.read(reinterpret_cast<char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        if (!file) {
            error = "failed to read image: " + resolved.Value().absolute;
            return false;
        }
        return true;
    }

    static std::string FloatKey(float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return std::to_string(bits);
    }

    static std::string BakeKey(const TextureUvTransform& transform,
                               const assets::TextureSampler& sampler, bool srgb) {
        return FloatKey(transform.offset.x) + ":" + FloatKey(transform.offset.y) + ":" +
               FloatKey(transform.rotation) + ":" + FloatKey(transform.scale.x) + ":" +
               FloatKey(transform.scale.y) + ":" +
               std::to_string(static_cast<int>(sampler.wrap_s)) + ":" +
               std::to_string(static_cast<int>(sampler.wrap_t)) + ":" +
               std::to_string(sampler.mag_filter == assets::TextureFilter::Linear ? 1 : 0) + ":" +
               (srgb ? "1" : "0");
    }

    static void SetSrgb(assets::JsonValue& settings, bool srgb) {
        for (auto& [key, value] : settings.object_value) {
            if (key == "sRGB") {
                value.bool_value = srgb;
                return;
            }
        }
    }

    [[nodiscard]] std::string Target() const {
        return request.settings != nullptr ? SettingString(*request.settings, "target", "") : "";
    }

    AssetGuid ResolveBaked(const cgltf_image* image, const TextureUvTransform& transform,
                           const assets::TextureSampler& sampler, bool srgb) {
        if (image == nullptr) {
            return {};
        }
        const std::size_t image_index = ImageIndex(image);
        const std::string key =
            std::to_string(image_index) + ":" + BakeKey(transform, sampler, srgb);
        if (const auto cached = baked_cache.find(key); cached != baked_cache.end()) {
            return cached->second;
        }

        std::vector<std::byte> bytes;
        std::string read_error;
        if (!ReadImageBytes(image, bytes, read_error)) {
            warnings.push_back("failed to bake glTF image '" + NameOr(image->name, "image") +
                               "': " + read_error);
            baked_cache.emplace(key, AssetGuid{});
            return {};
        }

        assets::JsonValue texture_settings = MakeDefaultImportSettings("texture", Target());
        SetSrgb(texture_settings, srgb);
        TextureBakeOptions bake;
        bake.transform = transform;
        bake.wrap_s = sampler.wrap_s;
        bake.wrap_t = sampler.wrap_t;
        bake.bilinear = sampler.mag_filter == assets::TextureFilter::Linear;
        auto decoded = DecodeTextureAsset(
            bytes, texture_settings,
            request.source_path + ":image:" + std::to_string(image_index) + ":bake", &bake);
        if (!decoded) {
            warnings.push_back("failed to bake glTF image '" + NameOr(image->name, "image") +
                               "': " + decoded.GetError().ToString());
            baked_cache.emplace(key, AssetGuid{});
            return {};
        }

        const std::string local_id = "gltf-image:" + std::to_string(image_index) + ":bake:" + key;
        const AssetGuid guid = MakeSubAssetGuid(request.source_guid, "texture", local_id);
        CookedAsset cooked;
        cooked.guid = guid;
        cooked.local_id = "texture:" + local_id;
        cooked.role = "texture";
        cooked.object = std::move(decoded).Value();
        assets.push_back(std::move(cooked));

        if (std::find(dependencies.begin(), dependencies.end(), guid) == dependencies.end()) {
            dependencies.push_back(guid);
        }
        baked_cache.emplace(key, guid);
        return guid;
    }
};
assets::TextureWrap ToTextureWrap(cgltf_wrap_mode wrap) {
    switch (wrap) {
    case cgltf_wrap_mode_clamp_to_edge:
        return assets::TextureWrap::Clamp;
    case cgltf_wrap_mode_mirrored_repeat:
        return assets::TextureWrap::Mirror;
    case cgltf_wrap_mode_repeat:
    default:
        return assets::TextureWrap::Repeat;
    }
}

assets::TextureFilter ToTextureFilter(cgltf_filter_type filter) {
    switch (filter) {
    case cgltf_filter_type_nearest:
    case cgltf_filter_type_nearest_mipmap_nearest:
    case cgltf_filter_type_nearest_mipmap_linear:
        return assets::TextureFilter::Nearest;
    case cgltf_filter_type_linear:
    case cgltf_filter_type_linear_mipmap_nearest:
    case cgltf_filter_type_linear_mipmap_linear:
    case cgltf_filter_type_undefined:
    default:
        return assets::TextureFilter::Linear;
    }
}

assets::TextureMipFilter ToTextureMipFilter(cgltf_filter_type filter) {
    switch (filter) {
    case cgltf_filter_type_nearest_mipmap_nearest:
    case cgltf_filter_type_linear_mipmap_nearest:
        return assets::TextureMipFilter::Nearest;
    default:
        return assets::TextureMipFilter::Linear;
    }
}

assets::TextureSampler ToTextureSampler(const cgltf_sampler* source) {
    assets::TextureSampler sampler;
    sampler.wrap_s = assets::TextureWrap::Repeat;
    sampler.wrap_t = assets::TextureWrap::Repeat;
    if (source != nullptr) {
        sampler.wrap_s = ToTextureWrap(source->wrap_s);
        sampler.wrap_t = ToTextureWrap(source->wrap_t);
        sampler.min_filter = ToTextureFilter(source->min_filter);
        sampler.mag_filter = ToTextureFilter(source->mag_filter);
        sampler.mip_filter = ToTextureMipFilter(source->min_filter);
    }
    return sampler;
}

struct MaterialUvTransformPlan {
    bool apply = false;
    bool bake_textures = false;
    bool requires_uv0 = false;
    TextureUvTransform transform;
    std::string unsupported_reason;
};

TextureUvTransform ViewTextureTransform(const cgltf_texture_view& view) {
    TextureUvTransform transform;
    if (view.has_transform) {
        transform.offset = {view.transform.offset[0], view.transform.offset[1]};
        transform.rotation = view.transform.rotation;
        transform.scale = {view.transform.scale[0], view.transform.scale[1]};
    }
    return transform;
}

MaterialUvTransformPlan PlanMaterialUvTransform(const cgltf_material* material) {
    MaterialUvTransformPlan result;
    if (material == nullptr) {
        return result;
    }

    const std::array<const cgltf_texture_view*, 5> views = {
        &material->pbr_metallic_roughness.base_color_texture,
        &material->pbr_metallic_roughness.metallic_roughness_texture,
        &material->normal_texture,
        &material->occlusion_texture,
        &material->emissive_texture,
    };

    bool has_any_transform = false;
    bool has_nonzero_uv_set = false;
    bool has_texture = false;
    bool transforms_mismatch = false;
    TextureUvTransform first;
    for (const cgltf_texture_view* view : views) {
        if (view->texture == nullptr) {
            continue;
        }
        has_any_transform |= view->has_transform != 0;
        const int uv_set = view->has_transform && view->transform.has_texcoord
                               ? view->transform.texcoord
                               : view->texcoord;
        has_nonzero_uv_set |= uv_set != 0;
        const TextureUvTransform transform = ViewTextureTransform(*view);
        if (!has_texture) {
            first = transform;
            has_texture = true;
        } else if (!TransformsMatch(first, transform)) {
            transforms_mismatch = true;
        }
    }
    if (!has_any_transform) {
        return result;
    }
    result.requires_uv0 = true;
    if (has_nonzero_uv_set) {
        result.unsupported_reason =
            "KHR_texture_transform requires TEXCOORD_0 for every material texture";
        return result;
    }
    if (!has_texture) {
        return result;
    }
    if (transforms_mismatch) {
        result.bake_textures = true;
        return result;
    }
    if (first.IsIdentity()) {
        return result;
    }
    result.apply = true;
    result.transform = first;
    return result;
}

AssetGuid ResolveTextureView(TextureResolver& resolver, const cgltf_texture_view& view,
                             assets::TextureRef& reference, bool bake_texture, bool srgb) {
    reference = {};
    if (view.texture == nullptr) {
        return {};
    }
    reference.sampler = ToTextureSampler(view.texture->sampler);
    const TextureUvTransform transform = ViewTextureTransform(view);
    if (bake_texture && !transform.IsIdentity()) {
        reference.texture =
            resolver.ResolveBaked(view.texture->image, transform, reference.sampler, srgb);
        if (!reference.texture.IsZero()) {
            reference.uv_set = 0;
            return reference.texture;
        }
    }
    reference.texture = resolver.Resolve(view.texture->image);
    reference.uv_set = view.has_transform && view.transform.has_texcoord
                           ? std::max(0, view.transform.texcoord)
                           : std::max(0, view.texcoord);
    return reference.texture;
}

struct PrimitiveInfo {
    const cgltf_primitive* source = nullptr;
    std::size_t vertex_offset = 0;
    std::size_t vertex_count = 0;
    std::size_t index_offset = 0;
    std::size_t index_count = 0;
    std::uint32_t material_index = 0;
    std::uint32_t target_count = 0;
    bool transform_uv = false;
    bool requires_uv0 = false;
    TextureUvTransform uv_transform;
};

struct MeshInfo {
    AssetGuid guid;
    std::shared_ptr<assets::MeshAsset> mesh;
    std::vector<AssetGuid> materials;
    std::uint32_t morph_target_count = 0;
};

const cgltf_material* SelectPrimitiveMaterial(const cgltf_primitive& primitive,
                                              std::optional<cgltf_size> material_variant) {
    if (material_variant.has_value()) {
        for (cgltf_size mapping_index = 0; mapping_index < primitive.mappings_count;
             ++mapping_index) {
            const cgltf_material_mapping& mapping = primitive.mappings[mapping_index];
            if (mapping.variant == *material_variant && mapping.material != nullptr) {
                return mapping.material;
            }
        }
    }
    return primitive.material;
}

const cgltf_scene* FindScene(const cgltf_data& data, std::string_view name) {
    if (name.empty()) {
        return data.scene;
    }
    for (cgltf_size index = 0; index < data.scenes_count; ++index) {
        const char* scene_name = data.scenes[index].name;
        if (scene_name != nullptr && name == scene_name) {
            return &data.scenes[index];
        }
    }
    bool numeric = true;
    std::uint64_t scene_index = 0;
    for (const char character : name) {
        if (character < '0' || character > '9') {
            numeric = false;
            break;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (scene_index >
            (std::numeric_limits<std::uint64_t>::max() - digit) / static_cast<std::uint64_t>(10)) {
            numeric = false;
            break;
        }
        scene_index = scene_index * static_cast<std::uint64_t>(10) + digit;
    }
    if (numeric && scene_index < static_cast<std::uint64_t>(data.scenes_count)) {
        return &data.scenes[static_cast<std::size_t>(scene_index)];
    }
    return nullptr;
}

std::unordered_set<const cgltf_node*> CollectSceneNodes(const cgltf_scene& scene) {
    std::unordered_set<const cgltf_node*> result;
    std::vector<const cgltf_node*> pending;
    pending.reserve(scene.nodes_count);
    for (cgltf_size index = 0; index < scene.nodes_count; ++index) {
        if (scene.nodes[index] != nullptr) {
            pending.push_back(scene.nodes[index]);
        }
    }
    while (!pending.empty()) {
        const cgltf_node* node = pending.back();
        pending.pop_back();
        if (node == nullptr || !result.insert(node).second) {
            continue;
        }
        for (cgltf_size child = 0; child < node->children_count; ++child) {
            if (node->children[child] != nullptr) {
                pending.push_back(node->children[child]);
            }
        }
    }
    return result;
}

void AddSkinInfluence(assets::SkinVertex& vertex, std::uint16_t joint, float weight) {
    if (!(weight > 0.0f) || !Finite(weight)) {
        return;
    }
    for (std::size_t influence = 0; influence < vertex.joints.size(); ++influence) {
        if (vertex.joints[influence] == joint) {
            vertex.weights[influence] += weight;
            return;
        }
    }
    std::size_t target = 0;
    for (std::size_t influence = 1; influence < vertex.joints.size(); ++influence) {
        if (vertex.weights[influence] < vertex.weights[target]) {
            target = influence;
        }
    }
    if (vertex.weights[target] <= 0.0f || weight > vertex.weights[target]) {
        vertex.joints[target] = joint;
        vertex.weights[target] = weight;
    }
}

void NormalizeSkinVertex(assets::SkinVertex& vertex) {
    float total = 0.0f;
    for (float weight : vertex.weights) {
        total += weight;
    }
    if (total <= 1.0e-8f) {
        vertex = {};
        return;
    }
    for (float& weight : vertex.weights) {
        weight /= total;
    }
}

struct MeshBuildResult {
    std::shared_ptr<assets::MeshAsset> mesh;
    std::uint32_t morph_target_count = 0;
    std::vector<AssetGuid> materials;
};

MeshBuildResult BuildMesh(
    const cgltf_mesh& source, std::size_t mesh_index, const std::vector<AssetGuid>& material_guids,
    const std::unordered_map<const cgltf_material*, std::uint32_t>& material_indices,
    const std::unordered_map<const cgltf_material*, MaterialUvTransformPlan>& material_uv_plans,
    std::uint32_t default_material, std::optional<cgltf_size> material_variant, float scale,
    std::vector<std::string>& warnings) {
    MeshBuildResult result;
    result.mesh = std::make_shared<assets::MeshAsset>();
    assets::MeshAsset& mesh = *result.mesh;
    mesh.name = NameOr(source.name, "Mesh" + std::to_string(mesh_index));

    std::vector<PrimitiveInfo> primitives;
    std::size_t total_vertices = 0;
    std::size_t total_indices = 0;
    std::uint32_t max_targets = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> material_remap;

    for (cgltf_size primitive_index = 0; primitive_index < source.primitives_count;
         ++primitive_index) {
        const cgltf_primitive& primitive = source.primitives[primitive_index];
        if (primitive.type != cgltf_primitive_type_triangles) {
            warnings.push_back("glTF mesh primitive is not a triangle list: " + mesh.name + "/" +
                               std::to_string(primitive_index));
            continue;
        }
        if (primitive.has_draco_mesh_compression) {
            warnings.push_back("glTF Draco primitive is not supported: " + mesh.name + "/" +
                               std::to_string(primitive_index));
            continue;
        }
        const cgltf_accessor* positions = FindAttribute(primitive, cgltf_attribute_type_position);
        if (positions == nullptr || positions->count == 0) {
            warnings.push_back("glTF mesh primitive has no positions: " + mesh.name + "/" +
                               std::to_string(primitive_index));
            continue;
        }
        if (positions->count > kMaxVertices || total_vertices + positions->count > kMaxVertices) {
            warnings.push_back("glTF mesh exceeds the vertex limit: " + mesh.name);
            break;
        }

        PrimitiveInfo info;
        info.source = &primitive;
        info.vertex_count = static_cast<std::size_t>(positions->count);
        info.vertex_offset = total_vertices;
        info.index_offset = total_indices;
        if (primitive.targets_count > kMaxPrimitiveTargets) {
            warnings.push_back("glTF mesh primitive has too many morph targets: " + mesh.name +
                               "/" + std::to_string(primitive_index));
        }
        info.target_count = static_cast<std::uint32_t>(
            std::min<cgltf_size>(primitive.targets_count, kMaxPrimitiveTargets));
        max_targets = std::max(max_targets, info.target_count);

        std::size_t primitive_index_count = info.vertex_count;
        if (primitive.indices != nullptr) {
            primitive_index_count = static_cast<std::size_t>(primitive.indices->count);
        }
        if (primitive_index_count > kMaxIndices ||
            total_indices + primitive_index_count > kMaxIndices) {
            warnings.push_back("glTF mesh exceeds the index limit: " + mesh.name);
            break;
        }
        info.index_count = primitive_index_count;

        const cgltf_material* material_source =
            SelectPrimitiveMaterial(primitive, material_variant);
        if (material_source != nullptr) {
            if (const auto plan = material_uv_plans.find(material_source);
                plan != material_uv_plans.end()) {
                info.requires_uv0 = plan->second.requires_uv0;
                if (plan->second.apply) {
                    info.transform_uv = true;
                    info.uv_transform = plan->second.transform;
                }
            }
        }
        std::uint32_t global_material = default_material;
        if (material_source != nullptr) {
            const auto found = material_indices.find(material_source);
            if (found != material_indices.end()) {
                global_material = found->second;
            } else {
                warnings.push_back("glTF primitive references an unknown material: " + mesh.name);
            }
        }
        std::uint32_t local_material = 0;
        if (const auto existing = material_remap.find(global_material);
            existing != material_remap.end()) {
            local_material = existing->second;
        } else {
            local_material = static_cast<std::uint32_t>(result.materials.size());
            material_remap.emplace(global_material, local_material);
            if (global_material < material_guids.size()) {
                result.materials.push_back(material_guids[global_material]);
            } else {
                result.materials.push_back({});
            }
        }
        info.material_index = local_material;

        primitives.push_back(info);
        total_vertices += info.vertex_count;
        total_indices += info.index_count;
    }

    if (primitives.empty()) {
        return result;
    }

    std::uint32_t target_count = max_targets;
    if (target_count != 0 && total_vertices != 0) {
        const std::uint64_t allowed =
            std::max<std::uint64_t>(1, assets::kMaxMorphDeltaVertices / total_vertices);
        if (target_count > allowed) {
            warnings.push_back(
                "glTF mesh exceeds the morph delta budget; excess targets are ignored: " +
                mesh.name);
        }
        target_count = static_cast<std::uint32_t>(std::min<std::uint64_t>(target_count, allowed));
    }
    if (target_count > assets::kMaxMorphTargets) {
        warnings.push_back("glTF mesh has too many morph targets: " + mesh.name);
        target_count = assets::kMaxMorphTargets;
    }

    mesh.vertices.resize(total_vertices);
    mesh.indices.reserve(total_indices);

    bool has_skin_attributes = false;
    std::size_t vertex_offset = 0;
    for (const PrimitiveInfo& info : primitives) {
        const cgltf_primitive& primitive = *info.source;
        const cgltf_accessor* positions = FindAttribute(primitive, cgltf_attribute_type_position);
        std::vector<float> position_data;
        std::size_t position_count = 0;
        if (!UnpackFloats(positions, 3, position_data, position_count) ||
            position_count != info.vertex_count) {
            warnings.push_back("glTF mesh has invalid positions: " + mesh.name);
            return result;
        }

        const cgltf_accessor* normals = FindAttribute(primitive, cgltf_attribute_type_normal);
        const cgltf_accessor* tangents = FindAttribute(primitive, cgltf_attribute_type_tangent);
        const cgltf_accessor* uv0 = FindAttribute(primitive, cgltf_attribute_type_texcoord, 0);
        const cgltf_accessor* uv1 = FindAttribute(primitive, cgltf_attribute_type_texcoord, 1);
        const cgltf_accessor* colors = FindAttribute(primitive, cgltf_attribute_type_color, 0);
        std::vector<float> normal_data;
        std::vector<float> tangent_data;
        std::vector<float> uv0_data;
        std::vector<float> uv1_data;
        std::vector<float> color_data;
        std::size_t normal_count = 0;
        std::size_t tangent_count = 0;
        std::size_t uv0_count = 0;
        std::size_t uv1_count = 0;
        std::size_t color_count = 0;
        const bool has_normals = UnpackFloats(normals, 3, normal_data, normal_count) &&
                                 normal_count == info.vertex_count;
        const bool has_tangents = UnpackFloats(tangents, 4, tangent_data, tangent_count) &&
                                  tangent_count == info.vertex_count;
        const bool has_uv0 =
            UnpackFloats(uv0, 2, uv0_data, uv0_count) && uv0_count == info.vertex_count;
        const bool has_uv1 =
            UnpackFloats(uv1, 2, uv1_data, uv1_count) && uv1_count == info.vertex_count;
        if (info.requires_uv0 && !has_uv0) {
            warnings.push_back("glTF KHR_texture_transform requires TEXCOORD_0 on primitive: " +
                               mesh.name);
        }
        bool has_colors = false;
        std::size_t color_components = 4;
        if (colors != nullptr) {
            std::vector<float> color_three;
            std::vector<float> color_four;
            std::size_t color_three_count = 0;
            std::size_t color_four_count = 0;
            if (UnpackFloats(colors, 4, color_four, color_four_count) &&
                color_four_count == info.vertex_count) {
                color_data = std::move(color_four);
                color_count = color_four_count;
                has_colors = true;
            } else if (UnpackFloats(colors, 3, color_three, color_three_count) &&
                       color_three_count == info.vertex_count) {
                color_data = std::move(color_three);
                color_count = color_three_count;
                color_components = 3;
                has_colors = true;
            }
        }

        for (std::size_t vertex = 0; vertex < info.vertex_count; ++vertex) {
            assets::MeshVertex& output = mesh.vertices[vertex_offset + vertex];
            const std::size_t position_index = vertex * 3;
            output.position = {position_data[position_index] * scale,
                               position_data[position_index + 1] * scale,
                               position_data[position_index + 2] * scale};
            if (has_normals) {
                output.normal = {normal_data[vertex * 3], normal_data[vertex * 3 + 1],
                                 normal_data[vertex * 3 + 2]};
            }
            if (has_tangents) {
                output.tangent = {tangent_data[vertex * 4], tangent_data[vertex * 4 + 1],
                                  tangent_data[vertex * 4 + 2], tangent_data[vertex * 4 + 3]};
            }
            if (has_uv0) {
                const Vec2 uv{uv0_data[vertex * 2], uv0_data[vertex * 2 + 1]};
                output.texcoord0 = info.transform_uv ? info.uv_transform.Apply(uv) : uv;
            }
            if (has_uv1) {
                output.texcoord1 = {uv1_data[vertex * 2], uv1_data[vertex * 2 + 1]};
            }
            if (has_colors) {
                const std::size_t offset = vertex * color_components;
                output.color = {color_data[offset], color_data[offset + 1], color_data[offset + 2],
                                color_components == 4 ? color_data[offset + 3] : 1.0f};
            }
        }

        std::vector<std::uint32_t> indices;
        if (primitive.indices != nullptr) {
            if (!UnpackIndices(primitive.indices, indices) || indices.size() != info.index_count) {
                warnings.push_back("glTF mesh has invalid indices: " + mesh.name);
                return result;
            }
        } else {
            indices.resize(info.vertex_count);
            for (std::size_t index = 0; index < info.vertex_count; ++index) {
                indices[index] = static_cast<std::uint32_t>(index);
            }
        }
        for (const std::uint32_t index : indices) {
            if (index >= info.vertex_count) {
                warnings.push_back("glTF mesh index is out of range: " + mesh.name);
                return result;
            }
            mesh.indices.push_back(static_cast<std::uint32_t>(info.vertex_offset + index));
        }

        std::map<int, std::pair<const cgltf_accessor*, const cgltf_accessor*>> skin_sets;
        for (cgltf_size attribute_index = 0; attribute_index < primitive.attributes_count;
             ++attribute_index) {
            const cgltf_attribute& attribute = primitive.attributes[attribute_index];
            if (attribute.type != cgltf_attribute_type_joints &&
                attribute.type != cgltf_attribute_type_weights) {
                continue;
            }
            auto& set = skin_sets[attribute.index];
            if (attribute.type == cgltf_attribute_type_joints) {
                set.first = attribute.data;
            } else {
                set.second = attribute.data;
            }
        }
        std::vector<std::pair<std::vector<float>, std::vector<float>>> valid_skin_sets;
        for (const auto& [set_index, accessors] : skin_sets) {
            (void)set_index;
            std::vector<float> joint_data;
            std::vector<float> weight_data;
            std::size_t joint_count = 0;
            std::size_t weight_count = 0;
            if (accessors.first == nullptr || accessors.second == nullptr ||
                !UnpackFloats(accessors.first, 4, joint_data, joint_count) ||
                !UnpackFloats(accessors.second, 4, weight_data, weight_count) ||
                joint_count != info.vertex_count || weight_count != info.vertex_count) {
                warnings.push_back("glTF mesh has an invalid skin influence set: " + mesh.name);
                continue;
            }
            valid_skin_sets.emplace_back(std::move(joint_data), std::move(weight_data));
        }
        if (!valid_skin_sets.empty()) {
            has_skin_attributes = true;
            if (mesh.skin_vertices.size() < total_vertices) {
                mesh.skin_vertices.resize(total_vertices);
            }
            for (std::size_t vertex = 0; vertex < info.vertex_count; ++vertex) {
                assets::SkinVertex& skin = mesh.skin_vertices[vertex_offset + vertex];
                for (std::size_t set = 0; set < valid_skin_sets.size(); ++set) {
                    for (std::size_t influence = 0; influence < 4; ++influence) {
                        const std::size_t offset = vertex * 4 + influence;
                        const float raw_joint = valid_skin_sets[set].first[offset];
                        const float raw_weight = valid_skin_sets[set].second[offset];
                        if (!Finite(raw_joint) || !Finite(raw_weight) || raw_weight <= 0.0f) {
                            continue;
                        }
                        const auto joint = static_cast<std::uint16_t>(std::clamp<std::int64_t>(
                            static_cast<std::int64_t>(std::llround(raw_joint)), 0, 65535));
                        AddSkinInfluence(skin, joint, raw_weight);
                    }
                }
                NormalizeSkinVertex(skin);
            }
        }

        assets::SubMesh submesh;
        submesh.index_offset = static_cast<std::uint32_t>(info.index_offset);
        submesh.index_count = static_cast<std::uint32_t>(info.index_count);
        submesh.material_index = info.material_index;
        Bounds bounds;
        for (const assets::MeshVertex& vertex : mesh.vertices) {
            if (!Finite(vertex.position) || !Finite(vertex.normal) || !Finite(vertex.tangent)) {
                warnings.push_back("glTF mesh has non-finite vertex data: " + mesh.name);
                result.mesh.reset();
                return result;
            }
        }

        Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max()};
        Vec3 maximum{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                     std::numeric_limits<float>::lowest()};
        for (std::size_t vertex = 0; vertex < info.vertex_count; ++vertex) {
            const Vec3& position = mesh.vertices[vertex_offset + vertex].position;
            minimum.x = std::min(minimum.x, position.x);
            minimum.y = std::min(minimum.y, position.y);
            minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x);
            maximum.y = std::max(maximum.y, position.y);
            maximum.z = std::max(maximum.z, position.z);
        }
        bounds.center = {(minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f,
                         (minimum.z + maximum.z) * 0.5f};
        bounds.extents = {(maximum.x - minimum.x) * 0.5f, (maximum.y - minimum.y) * 0.5f,
                          (maximum.z - minimum.z) * 0.5f};
        submesh.bounds = bounds;
        mesh.bounds.extents.x = std::max(mesh.bounds.extents.x, bounds.extents.x);
        mesh.bounds.extents.y = std::max(mesh.bounds.extents.y, bounds.extents.y);
        mesh.bounds.extents.z = std::max(mesh.bounds.extents.z, bounds.extents.z);
        mesh.sub_meshes.push_back(submesh);

        mesh.has_normals = mesh.has_normals || has_normals;
        mesh.has_tangents = mesh.has_tangents || has_tangents;
        mesh.has_uv0 = mesh.has_uv0 || has_uv0;
        mesh.has_uv1 = mesh.has_uv1 || has_uv1;
        mesh.has_colors = mesh.has_colors || has_colors;
        vertex_offset += info.vertex_count;
    }

    Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()};
    Vec3 maximum{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                 std::numeric_limits<float>::lowest()};
    for (const assets::MeshVertex& vertex : mesh.vertices) {
        minimum.x = std::min(minimum.x, vertex.position.x);
        minimum.y = std::min(minimum.y, vertex.position.y);
        minimum.z = std::min(minimum.z, vertex.position.z);
        maximum.x = std::max(maximum.x, vertex.position.x);
        maximum.y = std::max(maximum.y, vertex.position.y);
        maximum.z = std::max(maximum.z, vertex.position.z);
    }
    mesh.bounds.center = {(minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f,
                          (minimum.z + maximum.z) * 0.5f};
    mesh.bounds.extents = {(maximum.x - minimum.x) * 0.5f, (maximum.y - minimum.y) * 0.5f,
                           (maximum.z - minimum.z) * 0.5f};

    if (!mesh.skin_vertices.empty() && !has_skin_attributes) {
        mesh.skin_vertices.clear();
    }

    if (target_count != 0) {
        result.morph_target_count = target_count;
        mesh.morph_targets.resize(target_count);
        mesh.morph_weights.assign(target_count, 0.0f);
        for (std::uint32_t target_index = 0; target_index < target_count; ++target_index) {
            assets::MorphTarget& target = mesh.morph_targets[target_index];
            target.name = "Target " + std::to_string(target_index);
            if (source.target_names != nullptr && target_index < source.target_names_count &&
                source.target_names[target_index] != nullptr &&
                source.target_names[target_index][0] != '\0') {
                target.name = source.target_names[target_index];
            }
            target.position_deltas.resize(total_vertices);
        }
        if (source.weights != nullptr && source.weights_count == target_count) {
            for (std::uint32_t target_index = 0; target_index < target_count; ++target_index) {
                if (Finite(source.weights[target_index])) {
                    mesh.morph_weights[target_index] = source.weights[target_index];
                }
            }
        }

        for (const PrimitiveInfo& info : primitives) {
            const cgltf_primitive& primitive = *info.source;
            const cgltf_size primitive_targets =
                std::min<cgltf_size>(primitive.targets_count, target_count);
            for (cgltf_size target_index = 0; target_index < primitive_targets; ++target_index) {
                const cgltf_morph_target& source_target = primitive.targets[target_index];
                const cgltf_accessor* position_deltas =
                    FindMorphAttribute(source_target, cgltf_attribute_type_position);
                std::vector<float> delta_data;
                std::size_t delta_count = 0;
                if (UnpackFloats(position_deltas, 3, delta_data, delta_count) &&
                    delta_count == info.vertex_count) {
                    for (std::size_t vertex = 0; vertex < info.vertex_count; ++vertex) {
                        const std::size_t offset = vertex * 3;
                        mesh.morph_targets[target_index]
                            .position_deltas[info.vertex_offset + vertex] = {
                            delta_data[offset] * scale, delta_data[offset + 1] * scale,
                            delta_data[offset + 2] * scale};
                    }
                }
            }
        }

        for (std::uint32_t target_index = 0; target_index < target_count; ++target_index) {
            assets::MorphTarget& target = mesh.morph_targets[target_index];
            bool has_normal_deltas = false;
            bool has_tangent_deltas = false;
            for (const PrimitiveInfo& info : primitives) {
                if (target_index >= info.source->targets_count) {
                    continue;
                }
                const cgltf_morph_target& source_target = info.source->targets[target_index];
                if (FindMorphAttribute(source_target, cgltf_attribute_type_normal) != nullptr) {
                    has_normal_deltas = true;
                }
                if (FindMorphAttribute(source_target, cgltf_attribute_type_tangent) != nullptr) {
                    has_tangent_deltas = true;
                }
            }
            target.has_normals = has_normal_deltas;
            target.has_tangents = has_tangent_deltas;
            if (target.has_normals) {
                target.normal_deltas.resize(total_vertices);
            }
            if (target.has_tangents) {
                target.tangent_deltas.resize(total_vertices);
            }
        }

        for (const PrimitiveInfo& info : primitives) {
            const cgltf_primitive& primitive = *info.source;
            const cgltf_size primitive_targets =
                std::min<cgltf_size>(primitive.targets_count, target_count);
            for (cgltf_size target_index = 0; target_index < primitive_targets; ++target_index) {
                const cgltf_morph_target& source_target = primitive.targets[target_index];
                assets::MorphTarget& target = mesh.morph_targets[target_index];
                if (target.has_normals) {
                    std::vector<float> delta_data;
                    std::size_t delta_count = 0;
                    if (UnpackFloats(FindMorphAttribute(source_target, cgltf_attribute_type_normal),
                                     3, delta_data, delta_count) &&
                        delta_count == info.vertex_count) {
                        for (std::size_t vertex = 0; vertex < info.vertex_count; ++vertex) {
                            const std::size_t offset = vertex * 3;
                            target.normal_deltas[info.vertex_offset + vertex] = {
                                delta_data[offset], delta_data[offset + 1], delta_data[offset + 2]};
                        }
                    }
                }
                if (target.has_tangents) {
                    std::vector<float> delta_data;
                    std::size_t delta_count = 0;
                    if (UnpackFloats(
                            FindMorphAttribute(source_target, cgltf_attribute_type_tangent), 3,
                            delta_data, delta_count) &&
                        delta_count == info.vertex_count) {
                        for (std::size_t vertex = 0; vertex < info.vertex_count; ++vertex) {
                            const std::size_t offset = vertex * 3;
                            target.tangent_deltas[info.vertex_offset + vertex] = {
                                delta_data[offset], delta_data[offset + 1], delta_data[offset + 2],
                                0.0f};
                        }
                    }
                }
            }
        }
    }

    if (mesh.skin_vertices.size() > total_vertices) {
        mesh.skin_vertices.resize(total_vertices);
    }
    if (mesh.skin_vertices.size() != total_vertices) {
        mesh.skin_vertices.clear();
    }
    for (const assets::MorphTarget& target : mesh.morph_targets) {
        for (const Vec3& delta : target.position_deltas) {
            if (!Finite(delta)) {
                warnings.push_back("glTF mesh has non-finite morph positions: " + mesh.name);
                result.mesh.reset();
                return result;
            }
        }
        for (const Vec3& delta : target.normal_deltas) {
            if (!Finite(delta)) {
                warnings.push_back("glTF mesh has non-finite morph normals: " + mesh.name);
                result.mesh.reset();
                return result;
            }
        }
        for (const Vec4& delta : target.tangent_deltas) {
            if (!Finite(delta)) {
                warnings.push_back("glTF mesh has non-finite morph tangents: " + mesh.name);
                result.mesh.reset();
                return result;
            }
        }
    }

    return result;
}

std::optional<scene::SkinComponent>
BuildSkinComponent(const cgltf_skin* skin,
                   const std::unordered_map<const cgltf_node*, SceneEntityId>& node_entities,
                   std::vector<std::string>& warnings) {
    if (skin == nullptr || skin->joints_count == 0) {
        return std::nullopt;
    }
    if (skin->joints_count > 65536) {
        warnings.push_back("glTF skin has too many joints: " + NameOr(skin->name, "skin"));
        return std::nullopt;
    }
    scene::SkinComponent component;
    component.joints.reserve(skin->joints_count);
    component.inverse_bind_matrices.reserve(skin->joints_count);
    std::vector<Mat4> inverse_bind(skin->joints_count);
    if (skin->inverse_bind_matrices != nullptr) {
        std::vector<float> data;
        std::size_t count = 0;
        if (!UnpackFloats(skin->inverse_bind_matrices, 16, data, count) ||
            count < skin->joints_count) {
            warnings.push_back("glTF skin has invalid inverse bind matrices: " +
                               NameOr(skin->name, "skin"));
            return std::nullopt;
        }
        for (cgltf_size joint = 0; joint < skin->joints_count; ++joint) {
            inverse_bind[joint] = ToMat4(&data[joint * 16]);
        }
    } else {
        for (Mat4& matrix : inverse_bind) {
            matrix = Mat4{};
        }
    }
    for (const Mat4& matrix : inverse_bind) {
        for (float element : matrix.m) {
            if (!std::isfinite(element)) {
                warnings.push_back("glTF skin has non-finite inverse bind matrices: " +
                                   NameOr(skin->name, "skin"));
                return std::nullopt;
            }
        }
    }

    for (cgltf_size joint = 0; joint < skin->joints_count; ++joint) {
        const cgltf_node* node = skin->joints[joint];
        const auto entity = node_entities.find(node);
        if (entity == node_entities.end()) {
            warnings.push_back("glTF skin joint is not in the scene: " +
                               NameOr(skin->name, "skin"));
            return std::nullopt;
        }
        component.joints.push_back(entity->second);
        component.inverse_bind_matrices.push_back(inverse_bind[joint]);
    }
    return component;
}

template <typename T>
void AddComponent(SceneData& scene, scene::EntityId owner, const T& component) {
    auto payload = ComponentCodec<T>::Encode(component);
    if (!payload) {
        return;
    }
    scene::ComponentChunk* chunk = scene.FindComponent(ComponentCodec<T>::type_id);
    if (chunk == nullptr) {
        scene::ComponentChunk created;
        created.type = ComponentCodec<T>::type_id;
        created.schema_version = ComponentCodec<T>::schema_version;
        scene.components.push_back(std::move(created));
        chunk = &scene.components.back();
    }
    chunk->owners.push_back(SceneEntityId{owner});
    chunk->payloads.push_back(std::move(payload).Value());
}

template <typename T>
void SetComponent(SceneData& scene, scene::EntityId owner, const T& component) {
    auto payload = ComponentCodec<T>::Encode(component);
    if (!payload) {
        return;
    }
    scene::ComponentChunk* chunk = scene.FindComponent(ComponentCodec<T>::type_id);
    if (chunk == nullptr) {
        scene::ComponentChunk created;
        created.type = ComponentCodec<T>::type_id;
        created.schema_version = ComponentCodec<T>::schema_version;
        created.owners.push_back(SceneEntityId{owner});
        created.payloads.push_back(std::move(payload).Value());
        scene.components.push_back(std::move(created));
        return;
    }
    for (std::size_t index = 0; index < chunk->owners.size(); ++index) {
        if (chunk->owners[index].value == owner) {
            chunk->payloads[index] = std::move(payload).Value();
            return;
        }
    }
    chunk->owners.push_back(SceneEntityId{owner});
    chunk->payloads.push_back(std::move(payload).Value());
}

class GltfImporter final : public IAssetImporter {
  public:
    std::string_view Name() const noexcept override { return "gltf"; }
    std::uint32_t Version() const noexcept override { return 9; }

    bool Supports(std::string_view extension) const noexcept override {
        return extension == ".gltf" || extension == ".glb";
    }

    core::Result<ImportResult> Import(const ImportRequest& request) const override {
        const assets::JsonValue empty_settings;
        const assets::JsonValue& settings =
            request.settings != nullptr ? *request.settings : empty_settings;
        const double scale_value = SettingNumber(settings, "scale", 1.0);
        if (!(scale_value > 0.0) || !std::isfinite(scale_value) || scale_value > 100000.0) {
            return MakeError(ErrorCode::InvalidArgument, "glTF scale is invalid")
                .WithPath(request.source_path);
        }
        const float scale = static_cast<float>(scale_value);
        const bool optimize_mesh = SettingBool(settings, "optimizeMesh", true);
        const bool optimize_overdraw = SettingBool(settings, "optimizeOverdraw", false);
        const double overdraw_threshold = SettingNumber(settings, "overdrawThreshold", 1.05);
        if (optimize_mesh && optimize_overdraw &&
            (!std::isfinite(overdraw_threshold) || overdraw_threshold <= 0.0 ||
             overdraw_threshold > 100.0)) {
            return MakeError(ErrorCode::InvalidArgument, "glTF overdraw threshold is invalid")
                .WithPath(request.source_path);
        }

        cgltf_options options{};
        cgltf_data* raw_data = nullptr;
        const cgltf_result parse_result =
            cgltf_parse_file(&options, request.source_path.c_str(), &raw_data);
        if (parse_result != cgltf_result_success) {
            return MakeError(ErrorCode::Corrupt, "failed to parse glTF/GLB: " +
                                                     std::to_string(static_cast<int>(parse_result)))
                .WithPath(request.source_path);
        }
        std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw_data, cgltf_free);
        if (data->nodes_count > scene::kMaxSceneEntities) {
            return MakeError(ErrorCode::InvalidArgument, "glTF has too many nodes")
                .WithPath(request.source_path);
        }
        PARCEL_TRY(ValidateExternalBuffers(*data, request));
        if (cgltf_load_buffers(&options, data.get(), request.source_path.c_str()) !=
            cgltf_result_success) {
            return MakeError(ErrorCode::Io, "failed to load glTF buffers")
                .WithPath(request.source_path);
        }
        if (cgltf_validate(data.get()) != cgltf_result_success) {
            return MakeError(ErrorCode::Corrupt, "invalid glTF/GLB data")
                .WithPath(request.source_path);
        }
        for (cgltf_size extension_index = 0; extension_index < data->extensions_required_count;
             ++extension_index) {
            const char* extension = data->extensions_required[extension_index];
            if (extension == nullptr) {
                continue;
            }
            const std::string_view name(extension);
            if (name == "KHR_draco_mesh_compression" || name == "KHR_texture_basisu" ||
                name == "EXT_texture_webp") {
                return MakeError(ErrorCode::Unsupported,
                                 "required glTF extension is not supported: " + std::string(name))
                    .WithPath(request.source_path);
            }
        }
        PARCEL_TRY(DecodeMeshoptBufferViews(*data, request));
        const std::string material_variant_name = SettingString(settings, "materialVariant", "");
        std::optional<cgltf_size> material_variant;
        if (!material_variant_name.empty()) {
            for (cgltf_size index = 0; index < data->variants_count; ++index) {
                const char* name = data->variants[index].name;
                if (name != nullptr && material_variant_name == name) {
                    material_variant = index;
                    break;
                }
            }
            if (!material_variant.has_value()) {
                return MakeError(ErrorCode::InvalidArgument,
                                 "glTF material variant not found: " + material_variant_name)
                    .WithPath(request.source_path);
            }
        }
        const std::string scene_name = SettingString(settings, "scene", "");
        const cgltf_scene* selected_scene = FindScene(*data, scene_name);
        if (!scene_name.empty() && selected_scene == nullptr) {
            return MakeError(ErrorCode::InvalidArgument, "glTF scene not found: " + scene_name)
                .WithPath(request.source_path);
        }
        std::unordered_set<const cgltf_node*> selected_scene_nodes;
        if (selected_scene != nullptr) {
            selected_scene_nodes = CollectSceneNodes(*selected_scene);
        }
        const bool filter_scene_components = selected_scene != nullptr;
        if (data->meshes_count == 0 && data->cameras_count == 0 && data->animations_count == 0 &&
            data->nodes_count == 0) {
            return MakeError(ErrorCode::InvalidArgument, "glTF contains no importable scene data")
                .WithPath(request.source_path);
        }

        ImportResult result;
        std::vector<AssetGuid> dependencies;
        std::unordered_map<std::string, AssetGuid> textures_by_hash;
        std::vector<AssetGuid> material_guids;
        material_guids.reserve(data->materials_count + 1);
        std::unordered_map<const cgltf_material*, std::uint32_t> material_indices;
        std::unordered_map<std::string, AssetGuid> materials_by_content;
        TextureResolver texture_resolver{request, result.warnings,  dependencies,  {},
                                         {},      textures_by_hash, result.assets, data.get()};

        auto add_material = [&](const cgltf_material* source, std::size_t index) -> AssetGuid {
            TextureResolver& resolver = texture_resolver;
            assets::MaterialAsset material;
            if (source != nullptr) {
                material.name = NameOr(source->name, "Material" + std::to_string(index));
                material.type =
                    source->unlit ? assets::MaterialType::Unlit : assets::MaterialType::Lit;
                material.base_color_factor =
                    ToVec4(source->pbr_metallic_roughness.base_color_factor);
                material.metallic = source->pbr_metallic_roughness.metallic_factor;
                material.roughness = source->pbr_metallic_roughness.roughness_factor;
                material.emissive_factor = ToVec3(source->emissive_factor);
                material.emissive_strength = source->has_emissive_strength
                                                 ? source->emissive_strength.emissive_strength
                                                 : 1.0f;
                material.normal_scale = source->normal_texture.scale;
                material.occlusion_strength = source->occlusion_texture.scale;
                material.alpha_cutoff = source->alpha_cutoff;
                material.double_sided = source->double_sided != 0;
                switch (source->alpha_mode) {
                case cgltf_alpha_mode_mask:
                    material.alpha_mode = assets::AlphaMode::Mask;
                    material.render_queue = 2450;
                    break;
                case cgltf_alpha_mode_blend:
                    material.alpha_mode = assets::AlphaMode::Blend;
                    material.render_queue = 3000;
                    break;
                case cgltf_alpha_mode_opaque:
                default:
                    material.alpha_mode = assets::AlphaMode::Opaque;
                    material.render_queue = 2000;
                    break;
                }
                if (source->has_clearcoat) {
                    material.features |= assets::MaterialFeatureClearCoat;
                    material.clear_coat = source->clearcoat.clearcoat_factor;
                    material.clear_coat_roughness = source->clearcoat.clearcoat_roughness_factor;
                    material.clear_coat_normal_scale =
                        source->clearcoat.clearcoat_normal_texture.scale;
                }
                if (source->has_transmission) {
                    material.features |= assets::MaterialFeatureTransmission;
                    material.transmission = source->transmission.transmission_factor;
                }
                if (source->has_ior) {
                    material.features |= assets::MaterialFeatureIor;
                    material.ior = source->ior.ior;
                }
                if (source->has_iridescence) {
                    material.features |= assets::MaterialFeatureIridescence;
                    material.iridescence = source->iridescence.iridescence_factor;
                    material.iridescence_ior = source->iridescence.iridescence_ior;
                    material.iridescence_thickness = source->iridescence.iridescence_thickness_max;
                }
                if (source->has_anisotropy) {
                    material.features |= assets::MaterialFeatureAnisotropy;
                    material.anisotropy = source->anisotropy.anisotropy_strength;
                    material.anisotropy_rotation = source->anisotropy.anisotropy_rotation;
                }
                if (source->has_emissive_strength) {
                    material.features |= assets::MaterialFeatureEmissiveStrength;
                }
                if (source->has_diffuse_transmission) {
                    material.features |= assets::MaterialFeatureSubsurface;
                    material.subsurface_mask =
                        source->diffuse_transmission.diffuse_transmission_factor;
                    material.thickness = 1.0f;
                }
                const MaterialUvTransformPlan plan = PlanMaterialUvTransform(source);
                ResolveTextureView(resolver, source->pbr_metallic_roughness.base_color_texture,
                                   material.base_color_texture, plan.bake_textures, true);
                ResolveTextureView(resolver,
                                   source->pbr_metallic_roughness.metallic_roughness_texture,
                                   material.metallic_roughness_texture, plan.bake_textures, false);
                ResolveTextureView(resolver, source->normal_texture, material.normal_texture,
                                   plan.bake_textures, false);
                ResolveTextureView(resolver, source->occlusion_texture, material.occlusion_texture,
                                   plan.bake_textures, false);
                ResolveTextureView(resolver, source->emissive_texture, material.emissive_texture,
                                   plan.bake_textures, true);
            } else {
                material.name = "Default";
            }
            const AssetGuid guid = MakeSubAssetGuid(
                request.source_guid, "material",
                std::string("material:") + (source == nullptr ? "default" : std::to_string(index)));
            core::Hash256 content_hash = core::Sha256("gltf-material-v1");
            if (source != nullptr) {
                auto hashed = HashMaterialContent(material);
                if (hashed) {
                    content_hash = hashed.Value();
                }
            }
            if (const auto existing = materials_by_content.find(content_hash.ToHex());
                existing != materials_by_content.end()) {
                return existing->second;
            }
            materials_by_content.emplace(content_hash.ToHex(), guid);
            CookedAsset cooked;
            cooked.guid = guid;
            cooked.local_id =
                source == nullptr ? "material:default" : "material:" + std::to_string(index);
            cooked.role = "material";
            cooked.object = std::make_shared<assets::MaterialAsset>(std::move(material));
            cooked.dependencies = dependencies;
            result.assets.push_back(std::move(cooked));
            return guid;
        };

        for (cgltf_size index = 0; index < data->materials_count; ++index) {
            const AssetGuid guid = add_material(&data->materials[index], index);
            auto existing = std::find(material_guids.begin(), material_guids.end(), guid);
            if (existing == material_guids.end()) {
                material_guids.push_back(guid);
                existing = material_guids.end() - 1;
            }
            material_indices.emplace(
                &data->materials[index],
                static_cast<std::uint32_t>(std::distance(material_guids.begin(), existing)));
        }
        bool needs_default_material = data->materials_count == 0;
        for (cgltf_size mesh_index = 0; mesh_index < data->meshes_count && !needs_default_material;
             ++mesh_index) {
            const cgltf_mesh& mesh = data->meshes[mesh_index];
            for (cgltf_size primitive_index = 0; primitive_index < mesh.primitives_count;
                 ++primitive_index) {
                if (SelectPrimitiveMaterial(mesh.primitives[primitive_index], material_variant) ==
                    nullptr) {
                    needs_default_material = true;
                    break;
                }
            }
        }
        std::size_t default_material_index = 0;
        if (needs_default_material) {
            const AssetGuid default_material_guid = add_material(nullptr, 0);
            auto existing =
                std::find(material_guids.begin(), material_guids.end(), default_material_guid);
            if (existing == material_guids.end()) {
                default_material_index = material_guids.size();
                material_guids.push_back(default_material_guid);
            } else {
                default_material_index =
                    static_cast<std::size_t>(std::distance(material_guids.begin(), existing));
            }
        }

        std::unordered_map<const cgltf_material*, MaterialUvTransformPlan> material_uv_plans;
        for (cgltf_size index = 0; index < data->materials_count; ++index) {
            const MaterialUvTransformPlan plan = PlanMaterialUvTransform(&data->materials[index]);
            if (!plan.unsupported_reason.empty()) {
                result.warnings.push_back(
                    plan.unsupported_reason + ": " +
                    NameOr(data->materials[index].name, "Material" + std::to_string(index)));
            }
            if (plan.requires_uv0) {
                material_uv_plans.emplace(&data->materials[index], plan);
            }
        }

        std::vector<MeshInfo> mesh_infos(data->meshes_count);
        std::unordered_map<std::string, AssetGuid> meshes_by_content;
        std::unordered_set<AssetGuid, core::Guid128Hash> emitted_meshes;
        for (cgltf_size index = 0; index < data->meshes_count; ++index) {
            const cgltf_mesh& source = data->meshes[index];
            MeshBuildResult built =
                BuildMesh(source, static_cast<std::size_t>(index), material_guids, material_indices,
                          material_uv_plans, static_cast<std::uint32_t>(default_material_index),
                          material_variant, scale, result.warnings);
            if (!built.mesh || built.mesh->vertices.empty()) {
                result.warnings.push_back("glTF mesh is empty: " + std::to_string(index));
                continue;
            }
            if (optimize_mesh) {
                MeshOptimizeOptions optimize_options;
                optimize_options.optimize_overdraw = optimize_overdraw;
                optimize_options.overdraw_threshold = static_cast<float>(overdraw_threshold);
                auto optimized = OptimizeMesh(*built.mesh, optimize_options);
                if (!optimized) {
                    return std::move(optimized).TakeError().WithPath(request.source_path);
                }
            }
            AssetGuid guid;
            bool deduplicate = built.mesh->skin_vertices.empty();
            if (deduplicate) {
                auto hash = HashMeshContent(*built.mesh, built.materials);
                if (hash) {
                    const std::string key = hash.Value().ToHex();
                    if (const auto existing = meshes_by_content.find(key);
                        existing != meshes_by_content.end()) {
                        guid = existing->second;
                    } else {
                        guid = MakeSubAssetGuid(request.source_guid, "mesh",
                                                "mesh:" + std::to_string(index));
                        meshes_by_content.emplace(key, guid);
                    }
                }
            }
            if (guid.IsZero()) {
                guid =
                    MakeSubAssetGuid(request.source_guid, "mesh", "mesh:" + std::to_string(index));
            }
            mesh_infos[index].guid = guid;
            mesh_infos[index].mesh = built.mesh;
            mesh_infos[index].materials = std::move(built.materials);
            mesh_infos[index].morph_target_count = built.morph_target_count;
            if (!emitted_meshes.insert(guid).second) {
                continue;
            }
            CookedAsset cooked;
            cooked.guid = guid;
            cooked.local_id = "mesh:" + std::to_string(index);
            cooked.role = "mesh";
            cooked.object = built.mesh;
            cooked.dependencies = dependencies;
            result.assets.push_back(std::move(cooked));
        }

        auto scene_asset = std::make_shared<assets::SceneAsset>();
        scene_asset->name = std::string(core::path::Stem(request.source_path));
        SceneData& scene = scene_asset->scene;
        scene.entities.reserve(data->nodes_count);
        std::unordered_map<const cgltf_node*, SceneEntityId> node_entities;
        std::unordered_map<const cgltf_mesh*, std::size_t> mesh_indices;
        for (cgltf_size index = 0; index < data->meshes_count; ++index) {
            mesh_indices.emplace(&data->meshes[index], static_cast<std::size_t>(index));
        }
        node_entities.reserve(data->nodes_count);
        for (cgltf_size index = 0; index < data->nodes_count; ++index) {
            node_entities.emplace(&data->nodes[index],
                                  SceneEntityId{static_cast<scene::EntityId>(index + 1)});
        }
        for (cgltf_size index = 0; index < data->nodes_count; ++index) {
            const cgltf_node& node = data->nodes[index];
            scene::SceneEntityRecord record;
            record.id = node_entities[&node];
            record.parent = node.parent != nullptr ? node_entities[node.parent] : SceneEntityId{};
            record.name = NameOr(node.name, "Node" + std::to_string(index));
            record.transform = NodeTransform(node, scale);
            scene.entities.push_back(std::move(record));
        }
        for (cgltf_size index = 0; index < data->nodes_count; ++index) {
            const cgltf_node& node = data->nodes[index];
            const SceneEntityId entity = node_entities[&node];
            scene::TransformComponent transform;
            transform.position = scene.entities[index].transform.position;
            transform.rotation = scene.entities[index].transform.rotation;
            transform.scale = scene.entities[index].transform.scale;
            transform.parent = scene.entities[index].parent;
            AddComponent(scene, entity.value, transform);

            const bool in_scene = !filter_scene_components || selected_scene_nodes.contains(&node);
            if (in_scene && node.camera != nullptr) {
                scene::CameraComponent camera;
                if (node.camera->type == cgltf_camera_type_perspective) {
                    camera.projection = scene::CameraComponent::Projection::Perspective;
                    camera.yfov = node.camera->data.perspective.yfov;
                    camera.aspect = node.camera->data.perspective.has_aspect_ratio
                                        ? node.camera->data.perspective.aspect_ratio
                                        : 1.7777778f;
                    camera.near_clip = node.camera->data.perspective.znear;
                    camera.far_clip = node.camera->data.perspective.has_zfar
                                          ? node.camera->data.perspective.zfar
                                          : 1000.0f;
                } else if (node.camera->type == cgltf_camera_type_orthographic) {
                    camera.projection = scene::CameraComponent::Projection::Orthographic;
                    camera.xmag = node.camera->data.orthographic.xmag;
                    camera.ymag = node.camera->data.orthographic.ymag;
                    camera.near_clip = node.camera->data.orthographic.znear;
                    camera.far_clip = node.camera->data.orthographic.zfar;
                    camera.aspect = camera.xmag / std::max(1.0e-6f, camera.ymag);
                }
                AddComponent(scene, entity.value, camera);
            }

            if (in_scene && node.light != nullptr) {
                scene::LightComponent light;
                light.color = ToVec3(node.light->color);
                light.intensity = node.light->intensity;
                light.range = node.light->range;
                light.inner_cone_angle = node.light->spot_inner_cone_angle;
                light.outer_cone_angle = node.light->spot_outer_cone_angle;
                light.cast_shadows = true;
                switch (node.light->type) {
                case cgltf_light_type_directional:
                    light.type = scene::LightType::Directional;
                    break;
                case cgltf_light_type_spot:
                    light.type = scene::LightType::Spot;
                    break;
                case cgltf_light_type_point:
                default:
                    light.type = scene::LightType::Point;
                    break;
                }
                AddComponent(scene, entity.value, light);
            }

            if (in_scene && node.mesh != nullptr) {
                const auto mesh_index = mesh_indices.find(node.mesh);
                if (mesh_index != mesh_indices.end() && mesh_index->second < mesh_infos.size() &&
                    !mesh_infos[mesh_index->second].guid.IsZero()) {
                    MeshInfo& info = mesh_infos[mesh_index->second];
                    scene::MeshRendererComponent renderer;
                    renderer.mesh = info.guid;
                    renderer.materials = info.materials;
                    for (const assets::SubMesh& submesh : info.mesh->sub_meshes) {
                        renderer.submesh_materials.push_back(
                            static_cast<std::int32_t>(submesh.material_index));
                    }
                    AddComponent(scene, entity.value, renderer);

                    if (node.skin != nullptr) {
                        if (info.mesh == nullptr || info.mesh->skin_vertices.empty()) {
                            result.warnings.push_back("glTF node references a skin but its mesh "
                                                      "has no skin attributes: " +
                                                      scene.entities[index].name);
                        } else {
                            std::vector<std::string> skin_warnings;
                            const auto skin =
                                BuildSkinComponent(node.skin, node_entities, skin_warnings);
                            for (const std::string& warning : skin_warnings) {
                                result.warnings.push_back(warning);
                            }
                            if (skin.has_value()) {
                                bool valid_joints = true;
                                for (const assets::SkinVertex& vertex : info.mesh->skin_vertices) {
                                    for (std::size_t influence = 0;
                                         influence < vertex.joints.size(); ++influence) {
                                        if (vertex.weights[influence] > 0.0f &&
                                            vertex.joints[influence] >= skin->joints.size()) {
                                            valid_joints = false;
                                        }
                                    }
                                }
                                if (!valid_joints) {
                                    result.warnings.push_back(
                                        "glTF mesh skin joint index is out of range: " +
                                        scene.entities[index].name);
                                } else {
                                    AddComponent(scene, entity.value, *skin);
                                }
                            }
                        }
                    }

                    if (info.morph_target_count != 0) {
                        const std::size_t target_count = info.morph_target_count;
                        scene::MorphComponent morph;
                        if (node.weights != nullptr && node.weights_count == target_count) {
                            morph.weights.reserve(target_count);
                            for (std::size_t target = 0; target < target_count; ++target) {
                                const float weight = node.weights[target];
                                morph.weights.push_back(Finite(weight) ? weight : 0.0f);
                            }
                            AddComponent(scene, entity.value, morph);
                        } else if (info.mesh != nullptr && !info.mesh->morph_weights.empty()) {
                            morph.weights = info.mesh->morph_weights;
                            AddComponent(scene, entity.value, morph);
                        }
                    }
                }
            }
        }

        std::vector<AssetGuid> animation_guids;
        for (cgltf_size animation_index = 0; animation_index < data->animations_count;
             ++animation_index) {
            const cgltf_animation& source_animation = data->animations[animation_index];
            auto clip = std::make_shared<assets::AnimationClipAsset>();
            clip->name =
                NameOr(source_animation.name, "Animation" + std::to_string(animation_index));
            if (source_animation.channels_count > assets::kMaxAnimationChannels) {
                result.warnings.push_back("glTF animation has too many channels: " + clip->name);
                continue;
            }
            float maximum_time = 0.0f;
            for (cgltf_size channel_index = 0; channel_index < source_animation.channels_count;
                 ++channel_index) {
                const cgltf_animation_channel& source_channel =
                    source_animation.channels[channel_index];
                if (source_channel.target_node == nullptr || source_channel.sampler == nullptr) {
                    continue;
                }
                const auto target_entity = node_entities.find(source_channel.target_node);
                if (target_entity == node_entities.end()) {
                    result.warnings.push_back("glTF animation target is unknown: " + clip->name);
                    continue;
                }
                const cgltf_animation_sampler* sampler = source_channel.sampler;
                if (sampler->input == nullptr || sampler->output == nullptr ||
                    sampler->input->type != cgltf_type_scalar || sampler->input->count == 0) {
                    continue;
                }
                std::vector<float> time_data;
                std::size_t time_count = 0;
                if (!UnpackFloats(sampler->input, 1, time_data, time_count) || time_count == 0) {
                    continue;
                }
                if (time_count > assets::kMaxAnimationKeys) {
                    result.warnings.push_back("glTF animation has too many keys: " + clip->name);
                    continue;
                }
                bool sorted = true;
                for (std::size_t key = 0; key < time_count; ++key) {
                    if (!Finite(time_data[key]) || time_data[key] < 0.0f) {
                        sorted = false;
                        break;
                    }
                    if (key != 0 && time_data[key] < time_data[key - 1]) {
                        sorted = false;
                        break;
                    }
                }
                if (!sorted) {
                    result.warnings.push_back("glTF animation has invalid key times: " +
                                              clip->name);
                    continue;
                }
                const assets::AnimationInterpolation interpolation =
                    sampler->interpolation == cgltf_interpolation_type_step
                        ? assets::AnimationInterpolation::Step
                    : sampler->interpolation == cgltf_interpolation_type_cubic_spline
                        ? assets::AnimationInterpolation::CubicSpline
                        : assets::AnimationInterpolation::Linear;
                const bool cubic = interpolation == assets::AnimationInterpolation::CubicSpline;

                if (source_channel.target_path == cgltf_animation_path_type_weights) {
                    if (source_channel.target_node->mesh == nullptr) {
                        continue;
                    }
                    const auto morph_mesh = mesh_indices.find(source_channel.target_node->mesh);
                    if (morph_mesh == mesh_indices.end() ||
                        morph_mesh->second >= mesh_infos.size()) {
                        continue;
                    }
                    const std::size_t target_count =
                        mesh_infos[morph_mesh->second].morph_target_count;
                    if (target_count == 0) {
                        continue;
                    }
                    const std::size_t values_per_key = target_count * (cubic ? 3U : 1U);
                    if (sampler->output->count < time_count * values_per_key) {
                        result.warnings.push_back(
                            "glTF morph animation has an invalid output count: " + clip->name);
                        continue;
                    }
                    std::vector<float> weight_data;
                    std::size_t weight_count = 0;
                    if (!UnpackFloats(sampler->output, 1, weight_data, weight_count) ||
                        weight_count < time_count * values_per_key) {
                        continue;
                    }
                    for (std::size_t target = 0; target < target_count; ++target) {
                        assets::AnimationChannel channel;
                        channel.target = target_entity->second;
                        channel.path = assets::AnimationPath::Weights;
                        channel.interpolation = interpolation;
                        channel.target_index = static_cast<std::uint32_t>(target);
                        channel.times = time_data;
                        channel.values.resize(time_count);
                        if (cubic) {
                            channel.in_tangents.resize(time_count);
                            channel.out_tangents.resize(time_count);
                        }
                        for (std::size_t key = 0; key < time_count; ++key) {
                            const std::size_t base = key * values_per_key;
                            if (cubic) {
                                channel.in_tangents[key] =
                                    Vec4{weight_data[base + target], 0, 0, 0};
                                channel.values[key] =
                                    Vec4{weight_data[base + target_count + target], 0, 0, 0};
                                channel.out_tangents[key] =
                                    Vec4{weight_data[base + target_count * 2 + target], 0, 0, 0};
                            } else {
                                channel.values[key] = Vec4{weight_data[base + target], 0, 0, 0};
                            }
                        }
                        maximum_time = std::max(maximum_time, channel.times.back());
                        clip->channels.push_back(std::move(channel));
                    }
                    continue;
                }

                std::size_t output_components = 0;
                assets::AnimationPath path = assets::AnimationPath::Translation;
                if (source_channel.target_path == cgltf_animation_path_type_translation) {
                    path = assets::AnimationPath::Translation;
                    output_components = 3;
                } else if (source_channel.target_path == cgltf_animation_path_type_rotation) {
                    path = assets::AnimationPath::Rotation;
                    output_components = 4;
                } else if (source_channel.target_path == cgltf_animation_path_type_scale) {
                    path = assets::AnimationPath::Scale;
                    output_components = 3;
                } else {
                    continue;
                }
                const std::size_t key_stride = cubic ? 3U : 1U;
                const std::size_t required_floats = time_count * key_stride;
                if (sampler->output->count < required_floats) {
                    continue;
                }
                std::vector<float> output_data;
                std::size_t output_count = 0;
                if (!UnpackFloats(sampler->output, output_components, output_data, output_count) ||
                    output_count < required_floats) {
                    continue;
                }
                assets::AnimationChannel channel;
                channel.target = target_entity->second;
                channel.path = path;
                channel.interpolation = interpolation;
                channel.times = time_data;
                channel.values.resize(time_count);
                if (cubic) {
                    channel.in_tangents.resize(time_count);
                    channel.out_tangents.resize(time_count);
                }
                const std::size_t components = cgltf_num_components(sampler->output->type);
                const auto read_value = [&](std::size_t offset) {
                    if (path == assets::AnimationPath::Rotation) {
                        return Vec4{output_data[offset], output_data[offset + 1],
                                    output_data[offset + 2], output_data[offset + 3]};
                    }
                    return Vec4{output_data[offset], output_data[offset + 1],
                                output_data[offset + 2], 0.0f};
                };
                for (std::size_t key = 0; key < time_count; ++key) {
                    const std::size_t base = key * components * key_stride;
                    if (cubic) {
                        channel.in_tangents[key] = read_value(base);
                        channel.values[key] = read_value(base + components);
                        channel.out_tangents[key] = read_value(base + components * 2);
                    } else {
                        channel.values[key] = read_value(base);
                    }
                    if (path != assets::AnimationPath::Rotation) {
                        channel.values[key].x *= scale;
                        channel.values[key].y *= scale;
                        channel.values[key].z *= scale;
                        if (cubic) {
                            channel.in_tangents[key].x *= scale;
                            channel.in_tangents[key].y *= scale;
                            channel.in_tangents[key].z *= scale;
                            channel.out_tangents[key].x *= scale;
                            channel.out_tangents[key].y *= scale;
                            channel.out_tangents[key].z *= scale;
                        }
                    }
                }
                maximum_time = std::max(maximum_time, channel.times.back());
                clip->channels.push_back(std::move(channel));
            }
            if (clip->channels.empty()) {
                result.warnings.push_back("glTF animation has no importable channels: " +
                                          clip->name);
                continue;
            }
            clip->duration = maximum_time;
            if (!(clip->duration > 0.0f)) {
                result.warnings.push_back("glTF animation has no positive duration: " + clip->name);
                continue;
            }
            auto validation = assets::ValidateAnimationClip(*clip);
            if (!validation) {
                result.warnings.push_back("glTF animation is invalid: " + clip->name);
                continue;
            }
            const AssetGuid guid = MakeSubAssetGuid(request.source_guid, "animation",
                                                    "animation:" + std::to_string(animation_index));
            CookedAsset cooked;
            cooked.guid = guid;
            cooked.local_id = "animation:" + std::to_string(animation_index);
            cooked.role = "animation";
            cooked.object = std::move(clip);
            animation_guids.push_back(guid);
            result.assets.push_back(std::move(cooked));
        }

        if (!animation_guids.empty() && !scene.entities.empty()) {
            std::size_t root_index = 0;
            for (std::size_t index = 0; index < scene.entities.size(); ++index) {
                if (scene.entities[index].parent.IsRoot()) {
                    root_index = index;
                    break;
                }
            }
            SetComponent(scene, scene.entities[root_index].id.value,
                         scene::AnimationPlayerComponent{std::move(animation_guids)});
        }

        PARCEL_TRY(ApplySidecar(request, *scene_asset, textures_by_hash, result.warnings));

        const AssetGuid scene_guid = MakeSubAssetGuid(request.source_guid, "scene", "scene");
        CookedAsset cooked_scene;
        cooked_scene.guid = scene_guid;
        cooked_scene.local_id = "scene";
        cooked_scene.role = "scene";
        cooked_scene.object = std::move(scene_asset);
        result.assets.push_back(std::move(cooked_scene));
        for (const AssetGuid& dependency : dependencies) {
            if (!dependency.IsZero() &&
                std::find(result.dependencies.begin(), result.dependencies.end(), dependency) ==
                    result.dependencies.end()) {
                result.dependencies.push_back(dependency);
            }
        }
        return result;
    }
};

} // namespace

std::shared_ptr<const IAssetImporter> MakeGltfImporter() {
    return std::make_shared<GltfImporter>();
}

} // namespace parcel::import
