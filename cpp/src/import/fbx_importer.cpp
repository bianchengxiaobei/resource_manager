#include "parcel/import/content_hash.hpp"
#include "parcel/import/importer.hpp"
#include "parcel/import/mesh_optimizer.hpp"
#include "parcel/import/meta.hpp"
#include "parcel/import/sidecar_merge.hpp"
#include "parcel/import/texture_dedup.hpp"
#include "texture_decode.hpp"

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
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(PARCEL_IMPORT_ASSIMP) && PARCEL_IMPORT_ASSIMP
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif

namespace parcel::import {
namespace {

#if defined(PARCEL_IMPORT_ASSIMP) && PARCEL_IMPORT_ASSIMP

using core::ErrorCode;
using core::MakeError;
using scene::ComponentCodec;
using scene::SceneData;
using scene::SceneEntityId;

struct SkinInfo {
    std::vector<std::string> joint_names;
    std::vector<Mat4> inverse_bind_matrices;
};

struct MeshInfo {
    core::AssetGuid guid;
    std::vector<core::AssetGuid> materials;
    std::optional<SkinInfo> skin;
    std::shared_ptr<assets::MeshAsset> mesh;
    std::size_t morph_target_count = 0;
};

core::Result<std::string> ProjectRelative(std::string_view root, std::string_view path) {
    std::error_code error;
    const std::filesystem::path root_path =
        std::filesystem::path(std::u8string(root.begin(), root.end()));
    const std::filesystem::path value =
        std::filesystem::path(std::u8string(path.begin(), path.end()));
    const std::filesystem::path relative = std::filesystem::relative(value, root_path, error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to resolve texture path: " + error.message());
    }
    const std::u8string text = relative.u8string();
    std::string result =
        core::path::Normalize(std::string(reinterpret_cast<const char*>(text.data()), text.size()));
    if (result.rfind("../", 0) == 0 || result == "..") {
        return MakeError(ErrorCode::InvalidArgument, "texture path is outside the project");
    }
    return result;
}

struct TextureResolver {
    const ImportRequest& request;
    std::vector<std::string>& warnings;
    std::vector<core::AssetGuid>& dependencies;
    std::unordered_map<std::string, core::AssetGuid>& textures_by_hash;
    const aiScene* scene = nullptr;
    std::vector<CookedAsset>& assets;
    std::unordered_map<const aiTexture*, core::AssetGuid>& embedded_cache;

    [[nodiscard]] core::AssetGuid ResolveEmbedded(std::uint32_t texture_index,
                                                 const std::string_view text) {
        if (scene == nullptr || texture_index >= scene->mNumTextures) {
            warnings.push_back("embedded texture index is invalid: " + std::string(text));
            return {};
        }

        const aiTexture* texture = scene->mTextures[texture_index];
        if (texture == nullptr) {
            warnings.push_back("embedded texture is missing: " + std::string(text));
            return {};
        }
        if (const auto cached = embedded_cache.find(texture); cached != embedded_cache.end()) {
            return cached->second;
        }

        const std::string source_label =
            request.source_path + ":texture:" + std::to_string(texture_index);
        std::shared_ptr<assets::TextureAsset> cooked_texture;
        if (texture->mHeight == 0) {
            if (texture->mWidth == 0 || texture->pcData == nullptr) {
                warnings.push_back("embedded compressed texture is empty: " + std::string(text));
                embedded_cache.emplace(texture, core::AssetGuid{});
                return {};
            }
            const auto* begin = reinterpret_cast<const std::byte*>(texture->pcData);
            const std::span<const std::byte> bytes(begin, texture->mWidth);
            const std::string target =
                request.settings != nullptr ? SettingString(*request.settings, "target", "") : "";
            const assets::JsonValue texture_settings =
                MakeDefaultImportSettings("texture", target);
            auto decoded = DecodeTextureAsset(bytes, texture_settings, source_label);
            if (decoded) {
                cooked_texture = std::move(decoded).Value();
            } else if (request.texture_decode_fallback != nullptr) {
                cooked_texture = request.texture_decode_fallback(
                    begin, bytes.size(), request.texture_decode_user);
            }
            if (cooked_texture == nullptr) {
                warnings.push_back("failed to decode embedded texture '" + std::string(text) +
                                   "': " + decoded.GetError().ToString());
                embedded_cache.emplace(texture, core::AssetGuid{});
                return {};
            }
        } else {
            constexpr std::size_t kMaxEmbeddedDimension = 16384;
            constexpr std::size_t kMaxEmbeddedPixels = 64ull * 1000ull * 1000ull;
            const std::size_t width = texture->mWidth;
            const std::size_t height = texture->mHeight;
            if (width == 0 || height == 0 || texture->pcData == nullptr ||
                width > kMaxEmbeddedDimension || height > kMaxEmbeddedDimension ||
                width > kMaxEmbeddedPixels / height ||
                width > std::numeric_limits<std::uint32_t>::max() / 4u) {
                warnings.push_back("embedded raw texture dimensions are invalid: " +
                                   std::string(text));
                embedded_cache.emplace(texture, core::AssetGuid{});
                return {};
            }

            cooked_texture = std::make_shared<assets::TextureAsset>();
            cooked_texture->format = assets::TextureFormat::RGBA8;
            cooked_texture->sRGB = true;
            cooked_texture->mip_count = 1;
            assets::TextureMip mip;
            mip.width = static_cast<std::uint32_t>(width);
            mip.height = static_cast<std::uint32_t>(height);
            mip.row_pitch = static_cast<std::uint32_t>(width * 4u);
            mip.data.resize(width * height * 4u);
            for (std::size_t i = 0; i < width * height; ++i) {
                const aiTexel& texel = texture->pcData[i];
                mip.data[i * 4u + 0u] = static_cast<std::byte>(texel.r);
                mip.data[i * 4u + 1u] = static_cast<std::byte>(texel.g);
                mip.data[i * 4u + 2u] = static_cast<std::byte>(texel.b);
                mip.data[i * 4u + 3u] = static_cast<std::byte>(texel.a);
            }
            cooked_texture->mips.push_back(std::move(mip));
        }

        const core::AssetGuid guid = MakeSubAssetGuid(
            request.source_guid, "texture", "fbx-texture:" + std::to_string(texture_index));
        CookedAsset cooked;
        cooked.guid = guid;
        cooked.local_id = "texture:fbx:" + std::to_string(texture_index);
        cooked.role = "texture";
        cooked.object = std::move(cooked_texture);
        assets.push_back(std::move(cooked));
        if (std::find(dependencies.begin(), dependencies.end(), guid) == dependencies.end()) {
            dependencies.push_back(guid);
        }
        embedded_cache.emplace(texture, guid);
        return guid;
    }

    [[nodiscard]] core::AssetGuid Resolve(const aiString& value) {
        const std::string_view text(value.C_Str());
        if (text.empty()) {
            return {};
        }
        if (scene != nullptr) {
            const auto embedded = scene->GetEmbeddedTextureAndIndex(value.C_Str());
            if (embedded.first != nullptr && embedded.second >= 0) {
                return ResolveEmbedded(static_cast<std::uint32_t>(embedded.second), text);
            }
        }
        if (text.front() == '*') {
            warnings.push_back("embedded texture index is invalid: " + std::string(text));
            return {};
        }
        std::string texture_path = core::path::Normalize(text);
        std::string source_directory = core::path::Normalize(request.source_path);
        const auto slash = source_directory.find_last_of('/');
        source_directory =
            slash == std::string::npos ? std::string{} : source_directory.substr(0, slash);

        core::Result<std::string> candidate =
            core::path::IsAbsolute(texture_path) ? core::Result<std::string>{texture_path}
                                                 : core::path::Join(source_directory, texture_path);
        if (!candidate) {
            warnings.push_back("texture path escapes the source directory: " + std::string(text));
            return {};
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(
                std::filesystem::path(
                    std::u8string(candidate.Value().begin(), candidate.Value().end())),
                error)) {
            warnings.push_back("missing texture: " + candidate.Value());
            return {};
        }
        auto relative = ProjectRelative(request.project_root, candidate.Value());
        if (!relative) {
            warnings.push_back("texture is outside the project: " + candidate.Value());
            return {};
        }
        std::string content_key;
        if (const auto hash = core::Sha256File(candidate.Value()); hash) {
            content_key = hash.Value().ToHex();
        }
        core::AssetGuid guid;
        if (const TextureCanonicalAsset* canonical =
                FindCanonicalTextureByPath(request.texture_index, relative.Value());
            canonical != nullptr) {
            guid = canonical->asset_guid;
        } else if (!content_key.empty()) {
            const auto existing = textures_by_hash.find(content_key);
            if (existing != textures_by_hash.end()) {
                guid = existing->second;
            } else {
                guid = DefaultTextureAssetGuid(relative.Value());
                textures_by_hash.emplace(content_key, guid);
            }
        } else {
            guid = DefaultTextureAssetGuid(relative.Value());
        }
        if (std::find(dependencies.begin(), dependencies.end(), guid) == dependencies.end()) {
            dependencies.push_back(guid);
        }
        return guid;
    }
};

std::string FindSidecarPath(std::string_view source_path) {
    const std::string direct = std::string(source_path) + ".parcel.json";
    std::error_code error;
    if (std::filesystem::is_regular_file(
            std::filesystem::path(std::u8string(direct.begin(), direct.end())), error)) {
        return direct;
    }
    const std::string normalized = core::path::Normalize(source_path);
    const auto dot = normalized.find_last_of('.');
    const auto slash = normalized.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        const std::string alternate = normalized.substr(0, dot) + ".parcel.json";
        if (std::filesystem::is_regular_file(
                std::filesystem::path(std::u8string(alternate.begin(), alternate.end())), error)) {
            return alternate;
        }
    }
    return {};
}

core::AssetGuid ResolveProjectReference(std::string_view value, const ImportRequest& request,
                                        std::vector<std::string>& warnings,
                                        std::string_view context) {
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
    if (!std::filesystem::is_regular_file(
            std::filesystem::path(std::u8string(resolved.Value().begin(), resolved.Value().end())),
            error)) {
        warnings.push_back(std::string(context) + " is missing: " + resolved.Value());
        return {};
    }
    return MakeSourceGuid(relative);
}

aiColor3D MaterialColor(const aiMaterial& material, const char* key, unsigned int type,
                        unsigned int index, const aiColor3D& fallback) {
    aiColor3D value = fallback;
    if (material.Get(key, type, index, value) != AI_SUCCESS) {
        return fallback;
    }
    return value;
}

template <typename T>
core::Result<void> AddComponent(SceneData& data, scene::EntityId owner, const T& value) {
    auto payload = ComponentCodec<T>::Encode(value);
    if (!payload) {
        return std::move(payload).TakeError();
    }
    scene::ComponentChunk* chunk = data.FindComponent(ComponentCodec<T>::type_id);
    if (chunk == nullptr) {
        scene::ComponentChunk created;
        created.type = ComponentCodec<T>::type_id;
        created.schema_version = ComponentCodec<T>::schema_version;
        data.components.push_back(std::move(created));
        chunk = &data.components.back();
    }
    chunk->owners.push_back(SceneEntityId{owner});
    chunk->payloads.push_back(std::move(payload).Value());
    return {};
}

template <typename T>
core::Result<void> SetComponent(SceneData& data, scene::EntityId owner, const T& value) {
    auto payload = ComponentCodec<T>::Encode(value);
    if (!payload) {
        return std::move(payload).TakeError();
    }
    scene::ComponentChunk* chunk = data.FindComponent(ComponentCodec<T>::type_id);
    if (chunk == nullptr) {
        scene::ComponentChunk created;
        created.type = ComponentCodec<T>::type_id;
        created.schema_version = ComponentCodec<T>::schema_version;
        created.owners.push_back(SceneEntityId{owner});
        created.payloads.push_back(std::move(payload).Value());
        data.components.push_back(std::move(created));
        return {};
    }
    for (std::size_t i = 0; i < chunk->owners.size(); ++i) {
        if (chunk->owners[i].value == owner) {
            chunk->payloads[i] = std::move(payload).Value();
            return {};
        }
    }
    chunk->owners.push_back(SceneEntityId{owner});
    chunk->payloads.push_back(std::move(payload).Value());
    return {};
}

scene::TransformData DecomposeTransform(const aiMatrix4x4& matrix, float scale) {
    const float sx =
        std::sqrt(matrix.a1 * matrix.a1 + matrix.a2 * matrix.a2 + matrix.a3 * matrix.a3);
    const float sy =
        std::sqrt(matrix.b1 * matrix.b1 + matrix.b2 * matrix.b2 + matrix.b3 * matrix.b3);
    const float sz =
        std::sqrt(matrix.c1 * matrix.c1 + matrix.c2 * matrix.c2 + matrix.c3 * matrix.c3);
    const float safe_sx = sx > 1.0e-8f ? sx : 1.0f;
    const float safe_sy = sy > 1.0e-8f ? sy : 1.0f;
    const float safe_sz = sz > 1.0e-8f ? sz : 1.0f;
    aiMatrix3x3 rotation(matrix.a1 / safe_sx, matrix.a2 / safe_sx, matrix.a3 / safe_sx,
                         matrix.b1 / safe_sy, matrix.b2 / safe_sy, matrix.b3 / safe_sy,
                         matrix.c1 / safe_sz, matrix.c2 / safe_sz, matrix.c3 / safe_sz);
    aiQuaternion quaternion(rotation);
    scene::TransformData result;
    result.position = {matrix.a4 * scale, matrix.b4 * scale, matrix.c4 * scale};
    result.rotation = {quaternion.x, quaternion.y, quaternion.z, quaternion.w};
    result.scale = {sx * scale, sy * scale, sz * scale};
    return result;
}

float AnimationTimeScale(const aiAnimation& animation) {
    return animation.mTicksPerSecond > 0.0 ? 1.0f / static_cast<float>(animation.mTicksPerSecond)
                                           : 1.0f;
}

std::string_view StripMorphGeometrySuffix(std::string_view name) {
    const auto star = name.find_last_of('*');
    if (star == std::string_view::npos || star + 1 >= name.size()) {
        return name;
    }
    for (std::size_t index = star + 1; index < name.size(); ++index) {
        if (!std::isdigit(static_cast<unsigned char>(name[index]))) {
            return name;
        }
    }
    return name.substr(0, star);
}

scene::SceneEntityId
ResolveMorphChannelEntity(std::string_view name,
                          const std::unordered_map<std::string, scene::EntityId>& node_entity_ids) {
    const std::string_view base = StripMorphGeometrySuffix(name);
    const auto found = node_entity_ids.find(std::string(base));
    if (found != node_entity_ids.end()) {
        return SceneEntityId{found->second};
    }
    return {};
}

template <typename Key, typename Converter>
bool AppendAnimationChannel(const Key* keys, unsigned int count, float time_scale,
                            scene::SceneEntityId target, assets::AnimationPath path,
                            std::vector<assets::AnimationChannel>& channels, float& maximum_time,
                            Converter convert) {
    if (keys == nullptr || count == 0) {
        return false;
    }
    assets::AnimationChannel channel;
    channel.target = target;
    channel.path = path;
    channel.interpolation = assets::AnimationInterpolation::Linear;
    channel.times.reserve(count);
    channel.values.reserve(count);
    for (unsigned int index = 0; index < count; ++index) {
        const float time = static_cast<float>(keys[index].mTime) * time_scale;
        const Vec4 value = convert(keys[index].mValue);
        if (!std::isfinite(time) || time < 0.0f || !std::isfinite(value.x) ||
            !std::isfinite(value.y) || !std::isfinite(value.z) || !std::isfinite(value.w)) {
            continue;
        }
        if (!channel.times.empty() && time < channel.times.back()) {
            continue;
        }
        channel.times.push_back(time);
        channel.values.push_back(value);
        maximum_time = std::max(maximum_time, time);
    }
    if (channel.times.empty()) {
        return false;
    }
    channels.push_back(std::move(channel));
    return true;
}

bool AppendNodeAnimation(const aiNodeAnim& source, float time_scale, scene::SceneEntityId target,
                         std::vector<assets::AnimationChannel>& channels, float& maximum_time) {
    bool appended = false;
    appended |= AppendAnimationChannel(source.mPositionKeys, source.mNumPositionKeys, time_scale,
                                       target, assets::AnimationPath::Translation, channels,
                                       maximum_time, [](const aiVector3D& value) {
                                           return Vec4{value.x, value.y, value.z, 0.0f};
                                       });
    appended |= AppendAnimationChannel(source.mRotationKeys, source.mNumRotationKeys, time_scale,
                                       target, assets::AnimationPath::Rotation, channels,
                                       maximum_time, [](const aiQuaternion& value) {
                                           return Vec4{value.x, value.y, value.z, value.w};
                                       });
    appended |= AppendAnimationChannel(source.mScalingKeys, source.mNumScalingKeys, time_scale,
                                       target, assets::AnimationPath::Scale, channels, maximum_time,
                                       [](const aiVector3D& value) {
                                           return Vec4{value.x, value.y, value.z, 0.0f};
                                       });
    return appended;
}

parcel::Mat4 ToMat4(const aiMatrix4x4& source) {
    parcel::Mat4 result;
    for (unsigned int row = 0; row < 4; ++row) {
        for (unsigned int column = 0; column < 4; ++column) {
            result.m[column * 4 + row] = source[row][column];
        }
    }
    return result;
}

void AddSkinInfluence(assets::SkinVertex& vertex, std::uint16_t joint, float weight) {
    if (!(weight > 0.0f)) {
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

bool IsFinite(const aiVector3D& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

void BuildMorphTargets(const aiMesh& source, assets::MeshAsset& mesh, float scale,
                       std::vector<std::string>& warnings) {
    if (source.mNumAnimMeshes == 0 || source.mAnimMeshes == nullptr) {
        return;
    }
    std::size_t target_count =
        std::min<std::size_t>(source.mNumAnimMeshes, assets::kMaxMorphTargets);
    if (!mesh.vertices.empty()) {
        target_count = std::min<std::size_t>(target_count,
                                             assets::kMaxMorphDeltaVertices / mesh.vertices.size());
    }
    if (target_count < source.mNumAnimMeshes) {
        warnings.push_back("mesh has too many morph target deltas; only the first " +
                           std::to_string(target_count) + " targets are used: " + mesh.name);
    }
    if (target_count == 0) {
        return;
    }

    mesh.morph_targets.reserve(target_count);
    mesh.morph_weights.assign(target_count, 0.0f);
    for (std::size_t target_index = 0; target_index < target_count; ++target_index) {
        assets::MorphTarget target;
        target.name = "Target " + std::to_string(target_index);
        target.position_deltas.resize(mesh.vertices.size());
        const aiAnimMesh* source_target = source.mAnimMeshes[target_index];
        if (source_target == nullptr) {
            warnings.push_back("mesh morph target is missing: " + mesh.name + "/" +
                               std::to_string(target_index));
            mesh.morph_targets.push_back(std::move(target));
            continue;
        }
        if (source_target->mName.length != 0) {
            target.name = source_target->mName.C_Str();
        }
        const bool has_positions =
            source_target->HasPositions() && source_target->mNumVertices == source.mNumVertices;
        target.has_normals = source.HasNormals() && source_target->HasNormals() &&
                             source_target->mNumVertices == source.mNumVertices;
        target.has_tangents = source.HasTangentsAndBitangents() &&
                              source_target->HasTangentsAndBitangents() &&
                              source_target->mNumVertices == source.mNumVertices;
        if (!has_positions) {
            warnings.push_back("mesh morph target has no positions: " + mesh.name + "/" +
                               target.name);
        }
        if (target.has_normals) {
            target.normal_deltas.resize(mesh.vertices.size());
        }
        if (target.has_tangents) {
            target.tangent_deltas.resize(mesh.vertices.size());
        }
        for (unsigned int vertex = 0; vertex < source.mNumVertices; ++vertex) {
            if (has_positions) {
                const aiVector3D delta =
                    source_target->mVertices[vertex] - source.mVertices[vertex];
                if (IsFinite(delta)) {
                    target.position_deltas[vertex] = {delta.x * scale, delta.y * scale,
                                                      delta.z * scale};
                }
            }
            if (target.has_normals) {
                const aiVector3D delta = source_target->mNormals[vertex] - source.mNormals[vertex];
                if (IsFinite(delta)) {
                    target.normal_deltas[vertex] = {delta.x, delta.y, delta.z};
                }
            }
            if (target.has_tangents) {
                const aiVector3D delta =
                    source_target->mTangents[vertex] - source.mTangents[vertex];
                if (IsFinite(delta)) {
                    target.tangent_deltas[vertex] = {delta.x, delta.y, delta.z, 0.0f};
                }
            }
        }
        mesh.morph_targets.push_back(std::move(target));
    }
}

std::optional<scene::SkinComponent>
BuildSkinComponent(const SkinInfo& info, assets::MeshAsset& mesh,
                   const std::unordered_map<std::string, scene::EntityId>& node_entity_ids,
                   std::vector<std::string>& warnings) {
    scene::SkinComponent component;
    std::vector<std::int32_t> remap(info.joint_names.size(), -1);
    for (std::size_t joint_index = 0; joint_index < info.joint_names.size(); ++joint_index) {
        const auto found = node_entity_ids.find(info.joint_names[joint_index]);
        if (found == node_entity_ids.end()) {
            warnings.push_back("skin joint node is missing: " + info.joint_names[joint_index]);
            continue;
        }
        const auto existing = std::find(component.joints.begin(), component.joints.end(),
                                        SceneEntityId{found->second});
        if (existing != component.joints.end()) {
            remap[joint_index] =
                static_cast<std::int32_t>(std::distance(component.joints.begin(), existing));
            continue;
        }
        if (component.joints.size() >= 65536) {
            warnings.push_back("skin joint count exceeds limit: " + mesh.name);
            break;
        }
        remap[joint_index] = static_cast<std::int32_t>(component.joints.size());
        component.joints.push_back(SceneEntityId{found->second});
        component.inverse_bind_matrices.push_back(info.inverse_bind_matrices[joint_index]);
    }
    if (component.joints.empty()) {
        mesh.skin_vertices.clear();
        warnings.push_back("mesh skin has no resolvable joints: " + mesh.name);
        return std::nullopt;
    }

    bool has_weights = false;
    for (assets::SkinVertex& vertex : mesh.skin_vertices) {
        assets::SkinVertex remapped;
        for (std::size_t influence = 0; influence < vertex.joints.size(); ++influence) {
            const std::uint16_t source_joint = vertex.joints[influence];
            if (source_joint >= remap.size()) {
                continue;
            }
            const std::int32_t target = remap[source_joint];
            if (target >= 0) {
                AddSkinInfluence(remapped, static_cast<std::uint16_t>(target),
                                 vertex.weights[influence]);
            }
        }
        NormalizeSkinVertex(remapped);
        for (float weight : remapped.weights) {
            has_weights = has_weights || weight > 0.0f;
        }
        vertex = remapped;
    }
    if (!has_weights) {
        mesh.skin_vertices.clear();
        warnings.push_back("mesh skin has no valid weights: " + mesh.name);
        return std::nullopt;
    }
    return component;
}

class FbxImporter final : public IAssetImporter {
  public:
    std::string_view Name() const noexcept override { return "fbx"; }
    std::uint32_t Version() const noexcept override { return 8; }

    bool Supports(std::string_view extension) const noexcept override {
        return extension == ".fbx";
    }

    core::Result<ImportResult> Import(const ImportRequest& request) const override {
        const assets::JsonValue empty_settings;
        const assets::JsonValue& settings =
            request.settings != nullptr ? *request.settings : empty_settings;
        const double scale_value = SettingNumber(settings, "scale", 1.0);
        if (!(scale_value > 0.0) || scale_value > 100000.0) {
            return MakeError(ErrorCode::InvalidArgument, "FBX scale must be positive and finite");
        }
        const float scale = static_cast<float>(scale_value);
        const bool calculate_tangents = SettingBool(settings, "generateTangents", true);
        const bool optimize_mesh = SettingBool(settings, "optimizeMesh", true);
        const bool optimize_overdraw = SettingBool(settings, "optimizeOverdraw", false);
        const double overdraw_threshold = SettingNumber(settings, "overdrawThreshold", 1.05);
        if (optimize_mesh && optimize_overdraw &&
            (!std::isfinite(overdraw_threshold) || overdraw_threshold <= 0.0 ||
             overdraw_threshold > 100.0)) {
            return MakeError(ErrorCode::InvalidArgument, "FBX overdraw threshold is invalid");
        }

        Assimp::Importer importer;
        unsigned int flags =
            aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices;
        if (calculate_tangents) {
            flags |= aiProcess_CalcTangentSpace;
        }
        const aiScene* scene = importer.ReadFile(request.source_path, flags);
        if (scene == nullptr) {
            return MakeError(ErrorCode::Io, importer.GetErrorString())
                .WithPath(request.source_path);
        }
        if (scene->mNumMeshes == 0 && scene->mNumCameras == 0 && scene->mNumLights == 0 &&
            scene->mNumAnimations == 0) {
            return MakeError(ErrorCode::InvalidArgument, "FBX contains no importable scene data")
                .WithPath(request.source_path);
        }

        ImportResult result;

        std::vector<core::AssetGuid> material_guids;
        material_guids.reserve(scene->mNumMaterials);
        std::unordered_map<std::string, core::AssetGuid> materials_by_content;
        std::unordered_map<std::string, core::AssetGuid> textures_by_hash;
        std::unordered_map<const aiTexture*, core::AssetGuid> embedded_textures;
        for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
            const aiMaterial* source = scene->mMaterials[i];
            auto material = std::make_shared<assets::MaterialAsset>();
            material->name = std::string(source->GetName().C_Str());
            if (material->name.empty()) {
                material->name = "Material" + std::to_string(i);
            }
            aiColor3D base_color(1.0f, 1.0f, 1.0f);
            if (source->Get(AI_MATKEY_BASE_COLOR, base_color) != AI_SUCCESS) {
                base_color =
                    MaterialColor(*source, AI_MATKEY_COLOR_DIFFUSE, aiColor3D(1.0f, 1.0f, 1.0f));
            }
            material->base_color_factor = {base_color.r, base_color.g, base_color.b, 1.0f};
            float opacity = 1.0f;
            if (source->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 1.0f) {
                material->alpha_mode = assets::AlphaMode::Blend;
                material->base_color_factor.w = opacity;
            }
            float metallic = 0.0f;
            if (source->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS) {
                material->metallic = metallic;
            }
            float roughness = 1.0f;
            if (source->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) {
                material->roughness = roughness;
            }
            aiColor3D emissive(0.0f, 0.0f, 0.0f);
            if (source->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
                material->emissive_factor = {emissive.r, emissive.g, emissive.b};
            }
            int double_sided = 0;
            if (source->Get(AI_MATKEY_TWOSIDED, double_sided) == AI_SUCCESS) {
                material->double_sided = double_sided != 0;
            }

            std::vector<core::AssetGuid> dependencies;
            TextureResolver resolver{request, result.warnings, dependencies, textures_by_hash,
                                     scene, result.assets, embedded_textures};
            const std::pair<aiTextureType, assets::TextureRef*> textures[] = {
                {aiTextureType_BASE_COLOR, &material->base_color_texture},
                {aiTextureType_DIFFUSE, &material->base_color_texture},
                {aiTextureType_NORMALS, &material->normal_texture},
                {aiTextureType_METALNESS, &material->metallic_roughness_texture},
                {aiTextureType_DIFFUSE_ROUGHNESS, &material->metallic_roughness_texture},
                {aiTextureType_AMBIENT_OCCLUSION, &material->occlusion_texture},
                {aiTextureType_EMISSIVE, &material->emissive_texture},
            };
            for (const auto& [type, target] : textures) {
                aiString texture_path;
                if (source->GetTexture(type, 0, &texture_path) == AI_SUCCESS &&
                    target->texture.IsZero()) {
                    target->texture = resolver.Resolve(texture_path);
                }
            }
            core::AssetGuid guid =
                MakeSubAssetGuid(request.source_guid, "material", "material:" + std::to_string(i));
            PARCEL_TRY_DECL(core::Hash256, content_hash, HashMaterialContent(*material));
            const std::string content_key = content_hash.ToHex();
            if (const auto existing = materials_by_content.find(content_key);
                existing != materials_by_content.end()) {
                material_guids.push_back(existing->second);
                continue;
            }
            materials_by_content.emplace(content_key, guid);
            material_guids.push_back(guid);

            CookedAsset cooked;
            cooked.guid = guid;
            cooked.local_id = "material:" + std::to_string(i);
            cooked.role = "material";
            cooked.object = std::move(material);
            cooked.dependencies = std::move(dependencies);
            result.assets.push_back(std::move(cooked));
        }

        std::vector<MeshInfo> mesh_infos;
        mesh_infos.reserve(scene->mNumMeshes);
        std::unordered_map<std::string, core::AssetGuid> meshes_by_content;
        std::size_t valid_meshes = 0;
        for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
            const aiMesh* source = scene->mMeshes[i];
            if (source->mNumVertices == 0) {
                result.warnings.push_back("empty mesh: " + std::to_string(i));
                mesh_infos.push_back({});
                continue;
            }
            auto mesh = std::make_shared<assets::MeshAsset>();
            mesh->name =
                source->mName.length != 0 ? source->mName.C_Str() : "Mesh" + std::to_string(i);
            mesh->vertices.reserve(source->mNumVertices);
            const bool has_positions = source->HasPositions();
            const bool has_normals = source->HasNormals();
            const bool has_tangents = source->HasTangentsAndBitangents();
            const bool has_uv0 = source->HasTextureCoords(0);
            const bool has_uv1 = source->HasTextureCoords(1);
            const bool has_colors = source->HasVertexColors(0);
            mesh->has_normals = has_normals;
            mesh->has_tangents = has_tangents;
            mesh->has_uv0 = has_uv0;
            mesh->has_uv1 = has_uv1;
            mesh->has_colors = has_colors;
            float min_x = std::numeric_limits<float>::max();
            float min_y = std::numeric_limits<float>::max();
            float min_z = std::numeric_limits<float>::max();
            float max_x = std::numeric_limits<float>::lowest();
            float max_y = std::numeric_limits<float>::lowest();
            float max_z = std::numeric_limits<float>::lowest();
            for (unsigned int v = 0; v < source->mNumVertices; ++v) {
                assets::MeshVertex vertex;
                if (has_positions) {
                    vertex.position = {source->mVertices[v].x * scale,
                                       source->mVertices[v].y * scale,
                                       source->mVertices[v].z * scale};
                }
                if (has_normals) {
                    vertex.normal = {source->mNormals[v].x, source->mNormals[v].y,
                                     source->mNormals[v].z};
                }
                if (has_tangents) {
                    vertex.tangent = {source->mTangents[v].x, source->mTangents[v].y,
                                      source->mTangents[v].z, 1.0f};
                }
                if (has_uv0) {
                    vertex.texcoord0 = {source->mTextureCoords[0][v].x,
                                        source->mTextureCoords[0][v].y};
                }
                if (has_uv1) {
                    vertex.texcoord1 = {source->mTextureCoords[1][v].x,
                                        source->mTextureCoords[1][v].y};
                }
                if (has_colors) {
                    vertex.color = {source->mColors[0][v].r, source->mColors[0][v].g,
                                    source->mColors[0][v].b, source->mColors[0][v].a};
                }
                min_x = std::min(min_x, vertex.position.x);
                min_y = std::min(min_y, vertex.position.y);
                min_z = std::min(min_z, vertex.position.z);
                max_x = std::max(max_x, vertex.position.x);
                max_y = std::max(max_y, vertex.position.y);
                max_z = std::max(max_z, vertex.position.z);
                mesh->vertices.push_back(vertex);
            }
            mesh->indices.reserve(static_cast<std::size_t>(source->mNumFaces) * 3U);
            for (unsigned int f = 0; f < source->mNumFaces; ++f) {
                const aiFace& face = source->mFaces[f];
                if (face.mNumIndices != 3) {
                    result.warnings.push_back("non-triangle face was skipped");
                    continue;
                }
                for (unsigned int j = 0; j < face.mNumIndices; ++j) {
                    mesh->indices.push_back(face.mIndices[j]);
                }
            }
            std::optional<SkinInfo> skin_info;
            if (source->HasBones()) {
                SkinInfo skin;
                mesh->skin_vertices.assign(mesh->vertices.size(), {});
                bool has_weights = false;
                for (unsigned int bone_index = 0; bone_index < source->mNumBones; ++bone_index) {
                    const aiBone* bone = source->mBones[bone_index];
                    if (bone == nullptr || bone->mNumWeights == 0 || bone->mName.length == 0) {
                        continue;
                    }
                    if (skin.joint_names.size() >= 65536) {
                        result.warnings.push_back("mesh skin has too many bones: " +
                                                  std::string(source->mName.C_Str()));
                        break;
                    }
                    const auto joint = static_cast<std::uint16_t>(skin.joint_names.size());
                    skin.joint_names.emplace_back(bone->mName.C_Str());
                    skin.inverse_bind_matrices.push_back(ToMat4(bone->mOffsetMatrix));
                    for (unsigned int weight_index = 0; weight_index < bone->mNumWeights;
                         ++weight_index) {
                        const aiVertexWeight& source_weight = bone->mWeights[weight_index];
                        if (source_weight.mVertexId >= mesh->skin_vertices.size() ||
                            !std::isfinite(source_weight.mWeight) ||
                            source_weight.mWeight <= 0.0f) {
                            continue;
                        }
                        AddSkinInfluence(mesh->skin_vertices[source_weight.mVertexId], joint,
                                         source_weight.mWeight);
                        has_weights = true;
                    }
                }
                if (has_weights) {
                    for (assets::SkinVertex& vertex : mesh->skin_vertices) {
                        NormalizeSkinVertex(vertex);
                    }
                    skin_info = std::move(skin);
                } else {
                    mesh->skin_vertices.clear();
                    result.warnings.push_back("mesh has bones but no usable weights: " +
                                              std::string(source->mName.C_Str()));
                }
            }
            BuildMorphTargets(*source, *mesh, scale, result.warnings);
            mesh->bounds.center = {(min_x + max_x) * 0.5f, (min_y + max_y) * 0.5f,
                                   (min_z + max_z) * 0.5f};
            mesh->bounds.extents = {(max_x - min_x) * 0.5f, (max_y - min_y) * 0.5f,
                                    (max_z - min_z) * 0.5f};
            assets::SubMesh sub_mesh;
            sub_mesh.index_offset = 0;
            sub_mesh.index_count = static_cast<std::uint32_t>(mesh->indices.size());
            sub_mesh.material_index = 0;
            sub_mesh.bounds = mesh->bounds;
            mesh->sub_meshes.push_back(sub_mesh);

            if (optimize_mesh) {
                MeshOptimizeOptions optimize_options;
                optimize_options.optimize_overdraw = optimize_overdraw;
                optimize_options.overdraw_threshold = static_cast<float>(overdraw_threshold);
                auto optimized = OptimizeMesh(*mesh, optimize_options);
                if (!optimized) {
                    return std::move(optimized).TakeError().WithPath(request.source_path);
                }
            }

            MeshInfo info;
            info.guid = MakeSubAssetGuid(request.source_guid, "mesh", "mesh:" + std::to_string(i));
            info.morph_target_count = mesh->morph_targets.size();
            if (source->mMaterialIndex < material_guids.size()) {
                info.materials.push_back(material_guids[source->mMaterialIndex]);
            } else {
                result.warnings.push_back("mesh references an unknown material");
            }
            info.skin = std::move(skin_info);
            if (info.skin.has_value()) {
                info.mesh = mesh;
                mesh_infos.push_back(info);
                ++valid_meshes;
            } else {
                PARCEL_TRY_DECL(core::Hash256, content_hash,
                                HashMeshContent(*mesh, info.materials));
                const std::string content_key = content_hash.ToHex();
                if (const auto existing = meshes_by_content.find(content_key);
                    existing != meshes_by_content.end()) {
                    info.guid = existing->second;
                    mesh_infos.push_back(info);
                    ++valid_meshes;
                    continue;
                }
                meshes_by_content.emplace(content_key, info.guid);
                mesh_infos.push_back(info);
                ++valid_meshes;
            }

            CookedAsset cooked;
            cooked.guid = info.guid;
            cooked.local_id = "mesh:" + std::to_string(i);
            cooked.role = "mesh";
            cooked.object = std::move(mesh);
            result.assets.push_back(std::move(cooked));
        }
        if (scene->mNumMeshes != 0 && valid_meshes == 0) {
            return MakeError(ErrorCode::InvalidArgument, "FBX contains only empty meshes")
                .WithPath(request.source_path);
        }

        auto scene_asset = std::make_shared<assets::SceneAsset>();
        scene_asset->name = std::string(core::path::Stem(request.source_path));
        std::unordered_map<const aiNode*, scene::EntityId> node_ids;
        std::vector<const aiNode*> node_order;
        std::uint64_t next_id = 1;
        const std::function<void(const aiNode*, SceneEntityId)> add_node =
            [&](const aiNode* node, SceneEntityId parent) {
                const scene::EntityId id = next_id++;
                node_ids[node] = id;
                node_order.push_back(node);
                const scene::TransformData transform =
                    DecomposeTransform(node->mTransformation, scale);
                scene::SceneEntityRecord record;
                record.id = SceneEntityId{id};
                record.parent = parent;
                record.name =
                    node->mName.length != 0 ? node->mName.C_Str() : "Node" + std::to_string(id);
                record.transform = transform;
                scene_asset->scene.entities.push_back(record);
                for (unsigned int i = 0; i < node->mNumChildren; ++i) {
                    add_node(node->mChildren[i], SceneEntityId{id});
                }
            };
        if (scene->mRootNode != nullptr) {
            add_node(scene->mRootNode, SceneEntityId{});
        }
        std::unordered_map<std::string, scene::EntityId> node_entity_ids;
        node_entity_ids.reserve(node_order.size());
        for (const aiNode* node : node_order) {
            node_entity_ids.emplace(std::string(node->mName.C_Str()), node_ids[node]);
        }

        std::unordered_map<std::string, unsigned int> camera_by_name;
        for (unsigned int i = 0; i < scene->mNumCameras; ++i) {
            camera_by_name.emplace(scene->mCameras[i]->mName.C_Str(), i);
        }
        std::unordered_map<std::string, unsigned int> light_by_name;
        for (unsigned int i = 0; i < scene->mNumLights; ++i) {
            light_by_name.emplace(scene->mLights[i]->mName.C_Str(), i);
        }

        std::vector<std::optional<scene::SkinComponent>> mesh_skins(mesh_infos.size());
        std::vector<bool> mesh_skin_built(mesh_infos.size(), false);
        std::unordered_map<scene::EntityId, unsigned int> entity_mesh_index;
        for (const aiNode* node : node_order) {
            const scene::EntityId id = node_ids[node];
            const scene::TransformData transform = DecomposeTransform(node->mTransformation, scale);
            scene::TransformComponent transform_component;
            transform_component.position = transform.position;
            transform_component.rotation = transform.rotation;
            transform_component.scale = transform.scale;
            transform_component.parent = [&]() {
                const auto parent = node_ids.find(node->mParent);
                return parent == node_ids.end() ? SceneEntityId{} : SceneEntityId{parent->second};
            }();
            PARCEL_TRY(AddComponent(scene_asset->scene, id, transform_component));

            if (node->mNumMeshes != 0) {
                const unsigned int mesh_index = node->mMeshes[0];
                if (mesh_index < mesh_infos.size() && !mesh_infos[mesh_index].guid.IsZero()) {
                    entity_mesh_index.emplace(id, mesh_index);
                    scene::MeshRendererComponent renderer;
                    renderer.mesh = mesh_infos[mesh_index].guid;
                    renderer.materials = mesh_infos[mesh_index].materials;
                    for (std::size_t i = 0; i < renderer.materials.size(); ++i) {
                        renderer.submesh_materials.push_back(static_cast<std::int32_t>(i));
                    }
                    PARCEL_TRY(AddComponent(scene_asset->scene, id, renderer));
                    MeshInfo& mesh_info = mesh_infos[mesh_index];
                    if (mesh_info.skin.has_value()) {
                        if (!mesh_skin_built[mesh_index]) {
                            mesh_skin_built[mesh_index] = true;
                            if (mesh_info.mesh != nullptr) {
                                mesh_skins[mesh_index] =
                                    BuildSkinComponent(*mesh_info.skin, *mesh_info.mesh,
                                                       node_entity_ids, result.warnings);
                            }
                        }
                        if (mesh_skins[mesh_index].has_value()) {
                            PARCEL_TRY(
                                AddComponent(scene_asset->scene, id, *mesh_skins[mesh_index]));
                        }
                    }
                }
                if (node->mNumMeshes > 1) {
                    result.warnings.push_back("node has multiple meshes; only the first is used: " +
                                              std::string(node->mName.C_Str()));
                }
            }
            const auto camera_entry = camera_by_name.find(std::string(node->mName.C_Str()));
            if (camera_entry != camera_by_name.end()) {
                const unsigned int index = camera_entry->second;
                const aiCamera* camera = scene->mCameras[index];
                scene::CameraComponent component;
                component.projection = camera->mOrthographicWidth > 0.0f
                                           ? scene::CameraComponent::Projection::Orthographic
                                           : scene::CameraComponent::Projection::Perspective;
                component.yfov =
                    camera->mHorizontalFOV > 0.0f ? camera->mHorizontalFOV : 1.0471976f;
                component.aspect = camera->mAspect > 0.0f ? camera->mAspect : 1.7777778f;
                component.near_clip = camera->mClipPlaneNear;
                component.far_clip = camera->mClipPlaneFar;
                component.xmag = camera->mOrthographicWidth;
                component.ymag = camera->mOrthographicWidth / component.aspect;
                PARCEL_TRY(AddComponent(scene_asset->scene, id, component));
            }
            const auto light_entry = light_by_name.find(std::string(node->mName.C_Str()));
            if (light_entry != light_by_name.end()) {
                const unsigned int index = light_entry->second;
                const aiLight* light = scene->mLights[index];
                scene::LightComponent component;
                switch (light->mType) {
                case aiLightSource_DIRECTIONAL:
                    component.type = scene::LightType::Directional;
                    break;
                case aiLightSource_POINT:
                    component.type = scene::LightType::Point;
                    break;
                case aiLightSource_SPOT:
                    component.type = scene::LightType::Spot;
                    break;
                default:
                    component.type = scene::LightType::Area;
                    break;
                }
                const float intensity =
                    std::max(light->mColorDiffuse.r,
                             std::max(light->mColorDiffuse.g, light->mColorDiffuse.b));
                component.intensity = intensity;
                if (intensity > 1.0e-8f) {
                    component.color = {light->mColorDiffuse.r / intensity,
                                       light->mColorDiffuse.g / intensity,
                                       light->mColorDiffuse.b / intensity};
                } else {
                    component.color = {0.0f, 0.0f, 0.0f};
                }
                component.inner_cone_angle = light->mAngleInnerCone * 0.5f;
                component.outer_cone_angle = light->mAngleOuterCone * 0.5f;
                if (component.type == scene::LightType::Area) {
                    if (light->mSize.x > 0.0f) {
                        component.area_width = light->mSize.x;
                    }
                    if (light->mSize.y > 0.0f) {
                        component.area_height = light->mSize.y;
                    }
                }
                PARCEL_TRY(AddComponent(scene_asset->scene, id, component));
            }
        }

        std::vector<core::AssetGuid> animation_guids;
        for (unsigned int animation_index = 0; animation_index < scene->mNumAnimations;
             ++animation_index) {
            const aiAnimation* source_animation = scene->mAnimations[animation_index];
            if (source_animation == nullptr) {
                continue;
            }
            const float time_scale = AnimationTimeScale(*source_animation);
            std::vector<assets::AnimationChannel> channels;
            float maximum_time = 0.0f;
            for (unsigned int channel_index = 0; channel_index < source_animation->mNumChannels;
                 ++channel_index) {
                const aiNodeAnim* source_channel = source_animation->mChannels[channel_index];
                if (source_channel == nullptr) {
                    continue;
                }
                const auto target = node_entity_ids.find(source_channel->mNodeName.C_Str());
                if (target == node_entity_ids.end()) {
                    result.warnings.push_back("animation channel targets an unknown node: " +
                                              std::string(source_animation->mName.C_Str()) + "/" +
                                              std::string(source_channel->mNodeName.C_Str()));
                    continue;
                }
                AppendNodeAnimation(*source_channel, time_scale, SceneEntityId{target->second},
                                    channels, maximum_time);
            }
            std::map<std::pair<std::uint64_t, std::uint32_t>, std::vector<std::pair<float, float>>>
                morph_samples;
            for (unsigned int morph_index = 0;
                 morph_index < source_animation->mNumMorphMeshChannels; ++morph_index) {
                const aiMeshMorphAnim* source_morph =
                    source_animation->mMorphMeshChannels[morph_index];
                if (source_morph == nullptr || source_morph->mKeys == nullptr ||
                    source_morph->mNumKeys == 0) {
                    continue;
                }
                const SceneEntityId target =
                    ResolveMorphChannelEntity(source_morph->mName.C_Str(), node_entity_ids);
                if (target.IsRoot()) {
                    result.warnings.push_back("morph animation channel targets an unknown node: " +
                                              std::string(source_animation->mName.C_Str()) + "/" +
                                              std::string(source_morph->mName.C_Str()));
                    continue;
                }
                const auto mesh_entry = entity_mesh_index.find(target.value);
                if (mesh_entry == entity_mesh_index.end() ||
                    mesh_entry->second >= mesh_infos.size() ||
                    mesh_infos[mesh_entry->second].morph_target_count == 0) {
                    result.warnings.push_back("morph animation channel has no morph targets: " +
                                              std::string(source_animation->mName.C_Str()) + "/" +
                                              std::string(source_morph->mName.C_Str()));
                    continue;
                }
                const std::size_t target_count = mesh_infos[mesh_entry->second].morph_target_count;
                bool warned_target = false;
                for (unsigned int key_index = 0; key_index < source_morph->mNumKeys; ++key_index) {
                    const aiMeshMorphKey& key = source_morph->mKeys[key_index];
                    const float time = static_cast<float>(key.mTime) * time_scale;
                    if (!std::isfinite(time) || time < 0.0f || key.mNumValuesAndWeights == 0 ||
                        key.mValues == nullptr || key.mWeights == nullptr) {
                        continue;
                    }
                    for (unsigned int value_index = 0; value_index < key.mNumValuesAndWeights;
                         ++value_index) {
                        const std::uint32_t target_index = key.mValues[value_index];
                        const double source_weight = key.mWeights[value_index];
                        if (target_index >= target_count) {
                            if (!warned_target) {
                                result.warnings.push_back(
                                    "morph animation target index is out of range: " +
                                    std::string(source_animation->mName.C_Str()) + "/" +
                                    std::string(source_morph->mName.C_Str()));
                                warned_target = true;
                            }
                            continue;
                        }
                        if (!std::isfinite(source_weight)) {
                            continue;
                        }
                        morph_samples[{target.value, target_index}].push_back(
                            {time, static_cast<float>(source_weight)});
                    }
                }
            }
            for (auto& [key, samples] : morph_samples) {
                if (samples.empty()) {
                    continue;
                }
                std::stable_sort(
                    samples.begin(), samples.end(),
                    [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
                assets::AnimationChannel channel;
                channel.target = SceneEntityId{key.first};
                channel.path = assets::AnimationPath::Weights;
                channel.interpolation = assets::AnimationInterpolation::Linear;
                channel.target_index = key.second;
                channel.times.reserve(samples.size());
                channel.values.reserve(samples.size());
                for (const auto& [time, weight] : samples) {
                    if (!channel.times.empty() && time < channel.times.back()) {
                        continue;
                    }
                    channel.times.push_back(time);
                    channel.values.push_back({weight, 0.0f, 0.0f, 0.0f});
                    maximum_time = std::max(maximum_time, time);
                }
                if (!channel.times.empty()) {
                    channels.push_back(std::move(channel));
                }
            }
            if (channels.empty()) {
                result.warnings.push_back("animation has no importable channels: " +
                                          std::string(source_animation->mName.C_Str()));
                continue;
            }
            float duration = static_cast<float>(source_animation->mDuration) * time_scale;
            if (!std::isfinite(duration) || duration < maximum_time) {
                duration = maximum_time;
            }
            if (duration <= 0.0f) {
                result.warnings.push_back("animation has no positive duration: " +
                                          std::string(source_animation->mName.C_Str()));
                continue;
            }

            auto clip = std::make_shared<assets::AnimationClipAsset>();
            clip->name = source_animation->mName.length != 0
                             ? std::string(source_animation->mName.C_Str())
                             : "Animation" + std::to_string(animation_index);
            clip->duration = duration;
            clip->channels = std::move(channels);
            auto validation = assets::ValidateAnimationClip(*clip);
            if (!validation) {
                result.warnings.push_back("animation is invalid: " + clip->name + ": " +
                                          validation.GetError().message);
                continue;
            }

            CookedAsset cooked;
            cooked.guid = MakeSubAssetGuid(request.source_guid, "animation",
                                           "animation:" + std::to_string(animation_index));
            cooked.local_id = "animation:" + std::to_string(animation_index);
            cooked.role = "animation";
            cooked.object = std::move(clip);
            animation_guids.push_back(cooked.guid);
            result.assets.push_back(std::move(cooked));
        }

        if (!animation_guids.empty() && !node_order.empty()) {
            const auto root = node_ids.find(scene->mRootNode);
            const scene::EntityId animation_root =
                root == node_ids.end() ? node_ids[node_order.front()] : root->second;
            scene::AnimationPlayerComponent player;
            player.clips = std::move(animation_guids);
            PARCEL_TRY(SetComponent(scene_asset->scene, animation_root, player));
        }

        const std::string sidecar_path = FindSidecarPath(request.source_path);
        if (!sidecar_path.empty()) {
            auto sidecar = assets::LoadSceneSidecarFile(sidecar_path);
            if (!sidecar) {
                return std::move(sidecar).TakeError();
            }
            scene_asset->scene.pipeline_settings = ResolveProjectReference(
                sidecar.Value().settings.pipeline, request, result.warnings, "sidecar pipeline");
            scene_asset->scene.frame_settings = ResolveProjectReference(
                sidecar.Value().settings.frame, request, result.warnings, "sidecar frame");
            scene_asset->scene.post_process_profile =
                ResolveProjectReference(sidecar.Value().settings.post_process, request,
                                        result.warnings, "sidecar postProcess");
            scene_asset->scene.shadow_settings = ResolveProjectReference(
                sidecar.Value().settings.shadows, request, result.warnings, "sidecar shadows");
            scene_asset->scene.volumetric_fog_profile =
                ResolveProjectReference(sidecar.Value().settings.volumetric_fog, request,
                                        result.warnings, "sidecar volumetricFog");

            for (const assets::SidecarCamera& sidecar_camera : sidecar.Value().cameras) {
                const auto entity = std::find_if(
                    scene_asset->scene.entities.begin(), scene_asset->scene.entities.end(),
                    [&](const scene::SceneEntityRecord& value) {
                        return !sidecar_camera.name.empty() && value.name == sidecar_camera.name;
                    });
                if (entity == scene_asset->scene.entities.end()) {
                    result.warnings.push_back("sidecar camera has no matching node: " +
                                              sidecar_camera.name);
                    continue;
                }
                scene::CameraComponent component;
                component.projection =
                    sidecar_camera.projection == assets::SidecarProjection::Orthographic
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
                PARCEL_TRY(SetComponent(scene_asset->scene, entity->id.value, component));
            }
            SidecarMergeOptions merge_options;
            merge_options.project_root = request.project_root;
            merge_options.source_path = request.source_path;
            merge_options.texture_aliases = &textures_by_hash;
            merge_options.canonical_textures = request.texture_index;

            for (const assets::SidecarObject& object : sidecar.Value().lights) {
                const auto entity = std::find_if(
                    scene_asset->scene.entities.begin(), scene_asset->scene.entities.end(),
                    [&](const scene::SceneEntityRecord& value) {
                        return !object.name.empty() && value.name == object.name;
                    });
                if (entity == scene_asset->scene.entities.end()) {
                    result.warnings.push_back("sidecar light has no matching node: " + object.name);
                    continue;
                }
                scene::LightComponent component;
                if (const auto* type = object.data.Find("type");
                    type != nullptr && type->IsString()) {
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
                if (const auto* range = object.data.Find("range");
                    range != nullptr && range->IsNumber()) {
                    component.range = static_cast<float>(range->number_value);
                }
                if (const auto* shadows = object.data.Find("castShadows");
                    shadows != nullptr && shadows->IsBool()) {
                    component.cast_shadows = shadows->bool_value;
                }
                if (const auto* cookie = object.data.Find("cookie");
                    cookie != nullptr && cookie->IsString()) {
                    component.cookie = ResolveSidecarTexture(
                        cookie->string_value, merge_options,
                        "sidecar light '" + object.name + "' cookie", result.warnings);
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
                                                      "sidecar light '" + object.name + "' ies",
                                                      result.warnings);
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
                    component.cookie_offset = {
                        static_cast<float>(offset->array_value[0].number_value),
                        static_cast<float>(offset->array_value[1].number_value)};
                }
                PARCEL_TRY(SetComponent(scene_asset->scene, entity->id.value, component));
            }
            PARCEL_TRY(MergeSceneSidecar(scene_asset->scene, sidecar.Value(), merge_options,
                                         next_id, result.warnings));
        }

        const core::AssetGuid scene_guid = MakeSubAssetGuid(request.source_guid, "scene", "scene");
        CookedAsset cooked_scene;
        cooked_scene.guid = scene_guid;
        cooked_scene.local_id = "scene";
        cooked_scene.role = "scene";
        cooked_scene.object = std::move(scene_asset);
        result.assets.push_back(std::move(cooked_scene));
        return result;
    }
};

#endif

} // namespace

std::shared_ptr<const IAssetImporter> MakeFbxImporter() {
#if defined(PARCEL_IMPORT_ASSIMP) && PARCEL_IMPORT_ASSIMP
    return std::make_shared<FbxImporter>();
#else
    return nullptr;
#endif
}

} // namespace parcel::import
