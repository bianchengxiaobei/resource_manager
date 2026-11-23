#include "parcel/scene/scene_data.hpp"

#include "parcel/scene/component_codec.hpp"
#include "parcel/serialize/binary.hpp"
#include "parcel/serialize/format.hpp"
#include "parcel/serialize/math_io.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace parcel::scene {
namespace {

using core::ErrorCode;
using core::MakeError;
using serialize::BinaryReader;
using serialize::BinaryWriter;

constexpr std::uint32_t kMaxComponentPayloadBytes = 64U * 1024U * 1024U;

Result<std::uint32_t> ReadCount(BinaryReader& reader, std::uint32_t maximum, const char* what) {
    PARCEL_TRY_DECL(std::uint32_t, count, reader.ReadU32());
    if (count > maximum) {
        return MakeError(ErrorCode::Corrupt, std::string(what) + " count exceeds limit");
    }
    return count;
}

Result<void> Finish(BinaryReader& reader) {
    if (!reader.IsAtEnd()) {
        return MakeError(ErrorCode::Corrupt, "scene payload has trailing bytes");
    }
    return {};
}

Result<void> WriteHeader(BinaryWriter& writer, const SceneData& scene) {
    if (scene.entities.size() > std::numeric_limits<std::uint32_t>::max() ||
        scene.components.size() > std::numeric_limits<std::uint32_t>::max()) {
        return MakeError(ErrorCode::InvalidArgument, "scene table is too large");
    }
    PARCEL_TRY(
        writer.WriteBytes(std::as_bytes(std::span<const char>(kSceneMagic, sizeof(kSceneMagic)))));
    PARCEL_TRY(writer.WriteU16(kSceneVersion));
    PARCEL_TRY(writer.WriteU16(kSceneHeaderSize));
    PARCEL_TRY(writer.WriteU32(0));
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(scene.entities.size())));
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(scene.components.size())));
    PARCEL_TRY(writer.WriteU32(0));
    PARCEL_TRY(writer.WriteGuid128(scene.pipeline_settings));
    PARCEL_TRY(writer.WriteGuid128(scene.frame_settings));
    PARCEL_TRY(writer.WriteGuid128(scene.post_process_profile));
    PARCEL_TRY(writer.WriteGuid128(scene.shadow_settings));
    PARCEL_TRY(writer.WriteGuid128(scene.volumetric_fog_profile));
    PARCEL_TRY(writer.WriteZero(24));
    return {};
}

Result<void> WriteEntity(BinaryWriter& writer, const SceneEntityRecord& entity) {
    PARCEL_TRY(writer.WriteU64(entity.id.value));
    PARCEL_TRY(writer.WriteU64(entity.parent.value));
    PARCEL_TRY(writer.WriteString(entity.name));
    PARCEL_TRY(serialize::WriteVec3(writer, entity.transform.position));
    PARCEL_TRY(serialize::WriteQuat(writer, entity.transform.rotation));
    PARCEL_TRY(serialize::WriteVec3(writer, entity.transform.scale));
    PARCEL_TRY(writer.WriteU32(entity.layer));
    PARCEL_TRY(writer.WriteU32(entity.flags));
    return {};
}

Result<SceneEntityRecord> ReadEntity(BinaryReader& reader) {
    SceneEntityRecord entity;
    PARCEL_TRY_ASSIGN(entity.id.value, reader.ReadU64());
    PARCEL_TRY_ASSIGN(entity.parent.value, reader.ReadU64());
    PARCEL_TRY_DECL(std::string_view, name, reader.ReadString());
    entity.name.assign(name);
    PARCEL_TRY_ASSIGN(entity.transform.position, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(entity.transform.rotation, serialize::ReadQuat(reader));
    PARCEL_TRY_ASSIGN(entity.transform.scale, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(entity.layer, reader.ReadU32());
    PARCEL_TRY_ASSIGN(entity.flags, reader.ReadU32());
    return entity;
}

Result<void> WriteChunk(BinaryWriter& writer, const ComponentChunk& chunk) {
    if (chunk.owners.size() > std::numeric_limits<std::uint32_t>::max()) {
        return MakeError(ErrorCode::InvalidArgument, "component owner table is too large");
    }
    PARCEL_TRY(writer.WriteU32(chunk.type));
    PARCEL_TRY(writer.WriteU32(chunk.schema_version));
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(chunk.owners.size())));
    PARCEL_TRY(writer.WriteU32(0));
    for (const SceneEntityId owner : chunk.owners) {
        PARCEL_TRY(writer.WriteU64(owner.value));
    }
    for (const std::vector<std::byte>& payload : chunk.payloads) {
        if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
            return MakeError(ErrorCode::InvalidArgument, "component payload is too large");
        }
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(payload.size())));
        PARCEL_TRY(writer.WriteBytes(payload));
    }
    return {};
}

Result<ComponentChunk> ReadChunk(BinaryReader& reader, std::uint32_t count) {
    ComponentChunk chunk;
    PARCEL_TRY_ASSIGN(chunk.type, reader.ReadU32());
    PARCEL_TRY_ASSIGN(chunk.schema_version, reader.ReadU32());
    PARCEL_TRY_DECL(std::uint32_t, owner_count, ReadCount(reader, count, "component owner"));
    PARCEL_TRY(reader.Skip(4));
    if (owner_count > count) {
        return MakeError(ErrorCode::Corrupt, "component owner count exceeds component count");
    }
    chunk.owners.reserve(owner_count);
    for (std::uint32_t i = 0; i < owner_count; ++i) {
        PARCEL_TRY_DECL(std::uint64_t, owner, reader.ReadU64());
        chunk.owners.push_back(SceneEntityId{owner});
    }
    chunk.payloads.reserve(owner_count);
    for (std::uint32_t i = 0; i < owner_count; ++i) {
        PARCEL_TRY_DECL(std::uint32_t, payload_size, reader.ReadU32());
        if (payload_size == 0 || payload_size > kMaxComponentPayloadBytes) {
            return MakeError(ErrorCode::Corrupt, "component payload size is invalid");
        }
        PARCEL_TRY_DECL(std::span<const std::byte>, payload, reader.ReadBytes(payload_size));
        chunk.payloads.emplace_back(payload.begin(), payload.end());
    }
    return chunk;
}

} // namespace

const SceneEntityRecord* SceneData::FindEntity(SceneEntityId id) const noexcept {
    const auto it = std::find_if(entities.begin(), entities.end(),
                                 [id](const SceneEntityRecord& entity) { return entity.id == id; });
    return it == entities.end() ? nullptr : &*it;
}

SceneEntityRecord* SceneData::FindEntity(SceneEntityId id) noexcept {
    const auto it = std::find_if(entities.begin(), entities.end(),
                                 [id](const SceneEntityRecord& entity) { return entity.id == id; });
    return it == entities.end() ? nullptr : &*it;
}

const ComponentChunk* SceneData::FindComponent(ComponentTypeId type) const noexcept {
    const auto it =
        std::find_if(components.begin(), components.end(),
                     [type](const ComponentChunk& chunk) { return chunk.type == type; });
    return it == components.end() ? nullptr : &*it;
}

ComponentChunk* SceneData::FindComponent(ComponentTypeId type) noexcept {
    const auto it =
        std::find_if(components.begin(), components.end(),
                     [type](const ComponentChunk& chunk) { return chunk.type == type; });
    return it == components.end() ? nullptr : &*it;
}

Result<void> ComponentRegistry::Register(ComponentTypeInfo info) {
    if (info.id == 0 || info.name.empty() || info.schema_version == 0) {
        return MakeError(ErrorCode::InvalidArgument, "component type info is invalid");
    }
    for (const ComponentTypeInfo& existing : types_) {
        if (existing.id == info.id) {
            return MakeError(ErrorCode::InvalidArgument, "duplicate component type id");
        }
        if (existing.name == info.name) {
            return MakeError(ErrorCode::InvalidArgument, "duplicate component type name");
        }
    }
    types_.push_back(std::move(info));
    return {};
}

const ComponentTypeInfo* ComponentRegistry::Find(ComponentTypeId id) const noexcept {
    const auto it = std::find_if(types_.begin(), types_.end(),
                                 [id](const ComponentTypeInfo& info) { return info.id == id; });
    return it == types_.end() ? nullptr : &*it;
}

const ComponentTypeInfo* ComponentRegistry::Find(std::string_view name) const noexcept {
    const auto it =
        std::find_if(types_.begin(), types_.end(),
                     [name](const ComponentTypeInfo& info) { return info.name == name; });
    return it == types_.end() ? nullptr : &*it;
}

Result<void> ValidateScene(const SceneData& scene, const ComponentRegistry& registry) {
    if (scene.entities.size() > kMaxSceneEntities ||
        scene.components.size() > kMaxSceneComponents) {
        return MakeError(ErrorCode::InvalidArgument, "scene exceeds entity or component limit");
    }

    std::unordered_map<EntityId, const SceneEntityRecord*> entities;
    entities.reserve(scene.entities.size());
    for (const SceneEntityRecord& entity : scene.entities) {
        if (entity.id.IsRoot()) {
            return MakeError(ErrorCode::Corrupt, "scene entity id 0 is reserved");
        }
        if (!entities.emplace(entity.id.value, &entity).second) {
            return MakeError(ErrorCode::Corrupt, "duplicate scene entity id");
        }
    }
    for (const SceneEntityRecord& entity : scene.entities) {
        if (!entity.parent.IsRoot() && entities.find(entity.parent.value) == entities.end()) {
            return MakeError(ErrorCode::Corrupt, "scene entity parent does not exist");
        }
        if (entity.parent == entity.id) {
            return MakeError(ErrorCode::Corrupt, "scene entity cannot parent itself");
        }
    }
    for (const SceneEntityRecord& entity : scene.entities) {
        SceneEntityId current = entity.parent;
        std::uint64_t steps = 0;
        while (!current.IsRoot()) {
            const auto it = entities.find(current.value);
            if (it == entities.end()) {
                return MakeError(ErrorCode::Corrupt, "scene entity parent chain is broken");
            }
            if (it->second->parent == entity.id) {
                return MakeError(ErrorCode::Corrupt, "scene entity parent cycle");
            }
            current = it->second->parent;
            if (++steps > scene.entities.size()) {
                return MakeError(ErrorCode::Corrupt, "scene entity parent cycle");
            }
        }
    }

    std::unordered_set<std::uint32_t> component_types;
    for (const ComponentChunk& chunk : scene.components) {
        const ComponentTypeInfo* info = registry.Find(chunk.type);
        if (info == nullptr) {
            return MakeError(ErrorCode::Unsupported, "unknown component type id");
        }
        if (info->schema_version != chunk.schema_version) {
            return MakeError(ErrorCode::Unsupported, "unsupported component schema version");
        }
        if (!component_types.insert(chunk.type).second) {
            return MakeError(ErrorCode::Corrupt, "duplicate component chunk type");
        }
        if (chunk.owners.size() != chunk.payloads.size()) {
            return MakeError(ErrorCode::Corrupt, "component owners and payloads differ");
        }
        std::unordered_set<EntityId> owners;
        owners.reserve(chunk.owners.size());
        for (std::size_t i = 0; i < chunk.owners.size(); ++i) {
            if (entities.find(chunk.owners[i].value) == entities.end()) {
                return MakeError(ErrorCode::Corrupt, "component owner does not exist");
            }
            if (!owners.insert(chunk.owners[i].value).second) {
                return MakeError(ErrorCode::Corrupt, "duplicate component owner");
            }
            if (chunk.payloads[i].empty()) {
                return MakeError(ErrorCode::Corrupt, "component payload is empty");
            }
        }
    }
    return ValidateEntityReferences(scene, registry);
}

Result<void> ValidateEntityReferences(const SceneData& scene, const ComponentRegistry& registry) {
    const ComponentTypeInfo* transform_info = registry.Find(kTransformComponentType);
    if (transform_info == nullptr) {
        return {};
    }
    const ComponentChunk* chunk = scene.FindComponent(kTransformComponentType);
    if (chunk == nullptr) {
        return {};
    }
    for (std::size_t i = 0; i < chunk->payloads.size(); ++i) {
        PARCEL_TRY_DECL(TransformComponent, transform,
                        ComponentCodec<TransformComponent>::Decode(chunk->payloads[i]));
        if (!transform.parent.IsRoot() && scene.FindEntity(transform.parent) == nullptr) {
            return MakeError(ErrorCode::Corrupt, "transform parent entity does not exist");
        }
    }

    if (registry.Find(kSkinComponentType) == nullptr) {
        return {};
    }
    const ComponentChunk* skin_chunk = scene.FindComponent(kSkinComponentType);
    if (skin_chunk == nullptr) {
        return {};
    }
    for (const std::vector<std::byte>& payload : skin_chunk->payloads) {
        PARCEL_TRY_DECL(SkinComponent, skin, ComponentCodec<SkinComponent>::Decode(payload));
        for (const SceneEntityId joint : skin.joints) {
            if (scene.FindEntity(joint) == nullptr) {
                return MakeError(ErrorCode::Corrupt, "skin joint entity does not exist");
            }
        }
    }
    return {};
}

Result<void> CollectSceneReferences(const SceneData& scene, const ComponentRegistry& registry,
                                    std::vector<AssetGuid>& out) {
    PARCEL_TRY(ValidateScene(scene, registry));
    for (const ComponentChunk& chunk : scene.components) {
        const ComponentTypeInfo* info = registry.Find(chunk.type);
        if (info != nullptr && info->collect_references != nullptr) {
            for (const std::vector<std::byte>& payload : chunk.payloads) {
                PARCEL_TRY(info->collect_references(payload, out));
            }
        }
    }
    const AssetGuid settings[] = {scene.pipeline_settings, scene.frame_settings,
                                  scene.post_process_profile, scene.shadow_settings,
                                  scene.volumetric_fog_profile};
    for (const AssetGuid& guid : settings) {
        if (!guid.IsZero()) {
            out.push_back(guid);
        }
    }
    return {};
}

Result<std::vector<std::byte>> EncodeScene(const SceneData& scene,
                                           const ComponentRegistry& registry) {
    PARCEL_TRY(ValidateScene(scene, registry));
    BinaryWriter writer;
    PARCEL_TRY(WriteHeader(writer, scene));
    for (const SceneEntityRecord& entity : scene.entities) {
        PARCEL_TRY(WriteEntity(writer, entity));
    }
    for (const ComponentChunk& chunk : scene.components) {
        PARCEL_TRY(WriteChunk(writer, chunk));
    }
    return writer.Release();
}

Result<SceneData> DecodeScene(std::span<const std::byte> data, const ComponentRegistry& registry) {
    if (data.size() < kSceneHeaderSize) {
        return MakeError(ErrorCode::Corrupt, "scene file is smaller than its header");
    }
    BinaryReader reader(data);
    PARCEL_TRY_DECL(std::span<const std::byte>, magic, reader.ReadBytes(sizeof(kSceneMagic)));
    if (std::memcmp(magic.data(), kSceneMagic, sizeof(kSceneMagic)) != 0) {
        return MakeError(ErrorCode::Corrupt, "invalid scene magic");
    }
    PARCEL_TRY_DECL(std::uint16_t, version, reader.ReadU16());
    PARCEL_TRY_DECL(std::uint16_t, header_size, reader.ReadU16());
    if (version != kSceneVersion || header_size != kSceneHeaderSize) {
        return MakeError(ErrorCode::Unsupported, "unsupported scene format version");
    }
    PARCEL_TRY(reader.Skip(4));
    PARCEL_TRY_DECL(std::uint32_t, entity_count, ReadCount(reader, kMaxSceneEntities, "entity"));
    PARCEL_TRY_DECL(std::uint32_t, component_count,
                    ReadCount(reader, kMaxSceneComponents, "component"));
    PARCEL_TRY(reader.Skip(4));
    SceneData scene;
    PARCEL_TRY_ASSIGN(scene.pipeline_settings, reader.ReadGuid128());
    PARCEL_TRY_ASSIGN(scene.frame_settings, reader.ReadGuid128());
    PARCEL_TRY_ASSIGN(scene.post_process_profile, reader.ReadGuid128());
    PARCEL_TRY_ASSIGN(scene.shadow_settings, reader.ReadGuid128());
    PARCEL_TRY_ASSIGN(scene.volumetric_fog_profile, reader.ReadGuid128());
    PARCEL_TRY(reader.Skip(24));

    scene.entities.reserve(entity_count);
    for (std::uint32_t i = 0; i < entity_count; ++i) {
        PARCEL_TRY_DECL(SceneEntityRecord, entity, ReadEntity(reader));
        scene.entities.push_back(std::move(entity));
    }
    scene.components.reserve(component_count);
    for (std::uint32_t i = 0; i < component_count; ++i) {
        PARCEL_TRY_DECL(ComponentChunk, chunk, ReadChunk(reader, entity_count));
        scene.components.push_back(std::move(chunk));
    }
    PARCEL_TRY(Finish(reader));
    PARCEL_TRY(ValidateScene(scene, registry));
    return scene;
}

} // namespace parcel::scene
