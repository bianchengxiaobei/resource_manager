#include "parcel/scene/component_codec.hpp"

#include "parcel/scene/scene_data.hpp"
#include "parcel/serialize/math_io.hpp"

#include <cmath>
#include <limits>

namespace parcel::scene {
namespace {

using core::ErrorCode;
using core::MakeError;
using serialize::BinaryReader;
using serialize::BinaryWriter;

constexpr std::uint32_t kMaxComponentEntries = 1U * 1000U * 1000U;

template <typename T> Result<T> ReadEnum(BinaryReader& reader, std::uint8_t maximum) {
    PARCEL_TRY_DECL(std::uint8_t, raw, reader.ReadU8());
    if (raw > maximum) {
        return MakeError(ErrorCode::Corrupt, "enum value is out of range");
    }
    return static_cast<T>(raw);
}

template <typename T> Result<void> WriteEnum(BinaryWriter& writer, T value) {
    return writer.WriteU8(static_cast<std::uint8_t>(value));
}

Result<std::uint32_t> ReadEntryCount(BinaryReader& reader, std::uint32_t minimum_entry_size,
                                     const char* what) {
    PARCEL_TRY_DECL(std::uint32_t, count, reader.ReadU32());
    if (count > kMaxComponentEntries) {
        return MakeError(ErrorCode::Corrupt, std::string(what) + " count exceeds limit");
    }
    if (minimum_entry_size != 0 &&
        static_cast<std::uint64_t>(count) * minimum_entry_size > reader.Remaining()) {
        return MakeError(ErrorCode::Corrupt, std::string(what) + " count exceeds payload");
    }
    return count;
}

Result<void> Finish(BinaryReader& reader) {
    if (!reader.IsAtEnd()) {
        return MakeError(ErrorCode::Corrupt, "component payload has trailing bytes");
    }
    return {};
}

Result<void> WriteGuid(BinaryWriter& writer, const AssetGuid& guid) {
    return writer.WriteGuid128(guid);
}

Result<AssetGuid> ReadGuid(BinaryReader& reader) {
    AssetGuid guid;
    PARCEL_TRY_ASSIGN(guid, reader.ReadGuid128());
    return guid;
}

Result<void> WriteLightShadow(BinaryWriter& writer, const LightShadowSettings& value) {
    PARCEL_TRY(writer.WriteF32(value.dimmer));
    PARCEL_TRY(writer.WriteF32(value.fade_distance));
    PARCEL_TRY(writer.WriteF32(value.near_plane));
    PARCEL_TRY(writer.WriteU32(value.resolution));
    PARCEL_TRY(serialize::WriteVec3(writer, value.tint));
    PARCEL_TRY(writer.WriteBool(value.penumbra_tint));
    PARCEL_TRY(WriteEnum(writer, value.update_mode));
    PARCEL_TRY(writer.WriteBool(value.contact_shadows));
    PARCEL_TRY(writer.WriteF32(value.softness));
    PARCEL_TRY(writer.WriteU32(value.blocker_samples));
    PARCEL_TRY(writer.WriteU32(value.filter_samples));
    return {};
}

Result<LightShadowSettings> ReadLightShadow(BinaryReader& reader) {
    LightShadowSettings value;
    PARCEL_TRY_ASSIGN(value.dimmer, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.fade_distance, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.near_plane, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.resolution, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.tint, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.penumbra_tint, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.update_mode, ReadEnum<ShadowUpdateMode>(reader, 2));
    PARCEL_TRY_ASSIGN(value.contact_shadows, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.softness, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.blocker_samples, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.filter_samples, reader.ReadU32());
    return value;
}

void AddReference(std::vector<AssetGuid>& out, const AssetGuid& guid) {
    if (!guid.IsZero()) {
        out.push_back(guid);
    }
}

Result<void> CollectScriptReferences(const ScriptComponent& value, std::vector<AssetGuid>& out) {
    AddReference(out, value.script);
    for (const ScriptProperty& property : value.properties.properties) {
        if (property.type == PropertyType::Guid) {
            AddReference(out, property.guid_value);
        }
    }
    return {};
}

Result<void> CollectTransform(std::span<const std::byte>, std::vector<AssetGuid>&) { return {}; }

Result<void> CollectCamera(std::span<const std::byte>, std::vector<AssetGuid>&) { return {}; }

Result<void> CollectLight(std::span<const std::byte> payload, std::vector<AssetGuid>& out) {
    PARCEL_TRY_DECL(LightComponent, value, ComponentCodec<LightComponent>::Decode(payload));
    AddReference(out, value.cookie);
    AddReference(out, value.ies);
    return {};
}

Result<void> CollectMeshRenderer(std::span<const std::byte> payload, std::vector<AssetGuid>& out) {
    PARCEL_TRY_DECL(MeshRendererComponent, value,
                    ComponentCodec<MeshRendererComponent>::Decode(payload));
    AddReference(out, value.mesh);
    for (const AssetGuid& material : value.materials) {
        AddReference(out, material);
    }
    AddReference(out, value.lightmap_atlas);
    return {};
}

Result<void> CollectAnimationPlayer(std::span<const std::byte> payload,
                                    std::vector<AssetGuid>& out) {
    PARCEL_TRY_DECL(AnimationPlayerComponent, value,
                    ComponentCodec<AnimationPlayerComponent>::Decode(payload));
    for (const AssetGuid& clip : value.clips) {
        AddReference(out, clip);
    }
    return {};
}

Result<void> CollectSkin(std::span<const std::byte> payload, std::vector<AssetGuid>&) {
    PARCEL_TRY(ComponentCodec<SkinComponent>::Decode(payload));
    return {};
}

Result<void> CollectMorph(std::span<const std::byte> payload, std::vector<AssetGuid>&) {
    PARCEL_TRY(ComponentCodec<MorphComponent>::Decode(payload));
    return {};
}

Result<void> CollectDecal(std::span<const std::byte> payload, std::vector<AssetGuid>& out) {
    PARCEL_TRY_DECL(DecalComponent, value, ComponentCodec<DecalComponent>::Decode(payload));
    AddReference(out, value.albedo);
    AddReference(out, value.normal);
    AddReference(out, value.mask);
    return {};
}

Result<void> CollectProbe(std::span<const std::byte> payload, std::vector<AssetGuid>& out) {
    PARCEL_TRY_DECL(ReflectionProbeComponent, value,
                    ComponentCodec<ReflectionProbeComponent>::Decode(payload));
    AddReference(out, value.image);
    return {};
}

Result<void> CollectFog(std::span<const std::byte> payload, std::vector<AssetGuid>& out) {
    PARCEL_TRY_DECL(LocalFogVolumeComponent, value,
                    ComponentCodec<LocalFogVolumeComponent>::Decode(payload));
    AddReference(out, value.mask);
    return {};
}

Result<void> CollectAudio(std::span<const std::byte> payload, std::vector<AssetGuid>& out) {
    PARCEL_TRY_DECL(AudioSourceComponent, value,
                    ComponentCodec<AudioSourceComponent>::Decode(payload));
    AddReference(out, value.clip);
    return {};
}

Result<void> CollectAudioListener(std::span<const std::byte>, std::vector<AssetGuid>&) {
    return {};
}

Result<void> CollectRigidBody(std::span<const std::byte>, std::vector<AssetGuid>&) { return {}; }

Result<void> CollectCollider(std::span<const std::byte>, std::vector<AssetGuid>&) { return {}; }

Result<void> CollectScript(std::span<const std::byte> payload, std::vector<AssetGuid>& out) {
    PARCEL_TRY_DECL(ScriptComponent, value, ComponentCodec<ScriptComponent>::Decode(payload));
    return CollectScriptReferences(value, out);
}

} // namespace

Result<std::vector<std::byte>>
ComponentCodec<TransformComponent>::Encode(const TransformComponent& value) {
    BinaryWriter writer;
    PARCEL_TRY(serialize::WriteVec3(writer, value.position));
    PARCEL_TRY(serialize::WriteQuat(writer, value.rotation));
    PARCEL_TRY(serialize::WriteVec3(writer, value.scale));
    PARCEL_TRY(writer.WriteU64(value.parent.value));
    return writer.Release();
}

Result<TransformComponent>
ComponentCodec<TransformComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    TransformComponent value;
    PARCEL_TRY_ASSIGN(value.position, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.rotation, serialize::ReadQuat(reader));
    PARCEL_TRY_ASSIGN(value.scale, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.parent.value, reader.ReadU64());
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>>
ComponentCodec<CameraComponent>::Encode(const CameraComponent& value) {
    BinaryWriter writer;
    PARCEL_TRY(WriteEnum(writer, value.projection));
    PARCEL_TRY(writer.WriteF32(value.yfov));
    PARCEL_TRY(writer.WriteF32(value.aspect));
    PARCEL_TRY(writer.WriteF32(value.near_clip));
    PARCEL_TRY(writer.WriteF32(value.far_clip));
    PARCEL_TRY(writer.WriteF32(value.xmag));
    PARCEL_TRY(writer.WriteF32(value.ymag));
    PARCEL_TRY(serialize::WriteVec4(writer, value.clear_color));
    PARCEL_TRY(writer.WriteU32(value.culling_mask));
    PARCEL_TRY(writer.WriteI32(value.priority));
    return writer.Release();
}

Result<CameraComponent>
ComponentCodec<CameraComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    CameraComponent value;
    PARCEL_TRY_ASSIGN(value.projection, ReadEnum<CameraComponent::Projection>(reader, 1));
    PARCEL_TRY_ASSIGN(value.yfov, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.aspect, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.near_clip, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.far_clip, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.xmag, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.ymag, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.clear_color, serialize::ReadVec4(reader));
    PARCEL_TRY_ASSIGN(value.culling_mask, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.priority, reader.ReadI32());
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>> ComponentCodec<LightComponent>::Encode(const LightComponent& value) {
    BinaryWriter writer;
    PARCEL_TRY(WriteEnum(writer, value.type));
    PARCEL_TRY(serialize::WriteVec3(writer, value.color));
    PARCEL_TRY(writer.WriteF32(value.intensity));
    PARCEL_TRY(writer.WriteF32(value.range));
    PARCEL_TRY(writer.WriteF32(value.inner_cone_angle));
    PARCEL_TRY(writer.WriteF32(value.outer_cone_angle));
    PARCEL_TRY(WriteEnum(writer, value.area_shape));
    PARCEL_TRY(writer.WriteF32(value.area_width));
    PARCEL_TRY(writer.WriteF32(value.area_height));
    PARCEL_TRY(writer.WriteF32(value.area_shadow_cone));
    PARCEL_TRY(writer.WriteBool(value.cast_shadows));
    PARCEL_TRY(writer.WriteBool(value.shadow_configured));
    PARCEL_TRY(writer.WriteU16(value.light_layer_mask));
    PARCEL_TRY(WriteEnum(writer, value.cookie_mode));
    PARCEL_TRY(WriteEnum(writer, value.cookie_type));
    PARCEL_TRY(serialize::WriteVec2(writer, value.cookie_scale));
    PARCEL_TRY(serialize::WriteVec2(writer, value.cookie_offset));
    PARCEL_TRY(WriteGuid(writer, value.cookie));
    PARCEL_TRY(WriteGuid(writer, value.ies));
    PARCEL_TRY(writer.WriteBool(value.has_ies));
    PARCEL_TRY(writer.WriteF32(value.ies_cut));
    PARCEL_TRY(WriteLightShadow(writer, value.shadow));
    return writer.Release();
}

Result<LightComponent> ComponentCodec<LightComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    LightComponent value;
    PARCEL_TRY_ASSIGN(value.type, ReadEnum<LightType>(reader, 3));
    PARCEL_TRY_ASSIGN(value.color, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.intensity, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.range, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.inner_cone_angle, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.outer_cone_angle, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.area_shape, ReadEnum<LightAreaShape>(reader, 2));
    PARCEL_TRY_ASSIGN(value.area_width, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.area_height, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.area_shadow_cone, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.cast_shadows, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.shadow_configured, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.light_layer_mask, reader.ReadU16());
    PARCEL_TRY_ASSIGN(value.cookie_mode, ReadEnum<LightCookieMode>(reader, 2));
    PARCEL_TRY_ASSIGN(value.cookie_type, ReadEnum<LightCookieType>(reader, 2));
    PARCEL_TRY_ASSIGN(value.cookie_scale, serialize::ReadVec2(reader));
    PARCEL_TRY_ASSIGN(value.cookie_offset, serialize::ReadVec2(reader));
    PARCEL_TRY_ASSIGN(value.cookie, ReadGuid(reader));
    PARCEL_TRY_ASSIGN(value.ies, ReadGuid(reader));
    PARCEL_TRY_ASSIGN(value.has_ies, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.ies_cut, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.shadow, ReadLightShadow(reader));
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>>
ComponentCodec<MeshRendererComponent>::Encode(const MeshRendererComponent& value) {
    BinaryWriter writer;
    PARCEL_TRY(WriteGuid(writer, value.mesh));
    if (value.materials.size() > std::numeric_limits<std::uint32_t>::max()) {
        return MakeError(ErrorCode::InvalidArgument, "too many material references");
    }
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(value.materials.size())));
    for (const AssetGuid& material : value.materials) {
        PARCEL_TRY(WriteGuid(writer, material));
    }
    if (value.submesh_materials.size() > std::numeric_limits<std::uint32_t>::max()) {
        return MakeError(ErrorCode::InvalidArgument, "too many submesh material indices");
    }
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(value.submesh_materials.size())));
    for (std::int32_t index : value.submesh_materials) {
        PARCEL_TRY(writer.WriteI32(index));
    }
    PARCEL_TRY(writer.WriteBool(value.cast_shadows));
    PARCEL_TRY(writer.WriteBool(value.receive_shadows));
    PARCEL_TRY(writer.WriteI32(value.sorting_layer));
    PARCEL_TRY(writer.WriteI32(value.sorting_order));
    PARCEL_TRY(writer.WriteI32(value.renderer_priority));
    PARCEL_TRY(writer.WriteU16(value.light_layer_mask));
    PARCEL_TRY(writer.WriteU8(value.decal_layer_mask));
    PARCEL_TRY(WriteGuid(writer, value.lightmap_atlas));
    PARCEL_TRY(serialize::WriteVec4(writer, value.lightmap_st));
    PARCEL_TRY(serialize::WriteVec2(writer, value.lightmap_size));
    return writer.Release();
}

Result<MeshRendererComponent>
ComponentCodec<MeshRendererComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    MeshRendererComponent value;
    PARCEL_TRY_ASSIGN(value.mesh, ReadGuid(reader));
    PARCEL_TRY_DECL(std::uint32_t, material_count, ReadEntryCount(reader, 16, "material"));
    value.materials.reserve(material_count);
    for (std::uint32_t i = 0; i < material_count; ++i) {
        PARCEL_TRY_DECL(AssetGuid, material, ReadGuid(reader));
        value.materials.push_back(material);
    }
    PARCEL_TRY_DECL(std::uint32_t, submesh_count, ReadEntryCount(reader, 4, "submesh"));
    value.submesh_materials.reserve(submesh_count);
    for (std::uint32_t i = 0; i < submesh_count; ++i) {
        PARCEL_TRY_DECL(std::int32_t, index, reader.ReadI32());
        value.submesh_materials.push_back(index);
    }
    PARCEL_TRY_ASSIGN(value.cast_shadows, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.receive_shadows, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.sorting_layer, reader.ReadI32());
    PARCEL_TRY_ASSIGN(value.sorting_order, reader.ReadI32());
    PARCEL_TRY_ASSIGN(value.renderer_priority, reader.ReadI32());
    PARCEL_TRY_ASSIGN(value.light_layer_mask, reader.ReadU16());
    PARCEL_TRY_ASSIGN(value.decal_layer_mask, reader.ReadU8());
    PARCEL_TRY_ASSIGN(value.lightmap_atlas, ReadGuid(reader));
    PARCEL_TRY_ASSIGN(value.lightmap_st, serialize::ReadVec4(reader));
    PARCEL_TRY_ASSIGN(value.lightmap_size, serialize::ReadVec2(reader));
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>>
ComponentCodec<AnimationPlayerComponent>::Encode(const AnimationPlayerComponent& value) {
    if (value.clips.size() > kMaxComponentEntries) {
        return MakeError(ErrorCode::InvalidArgument, "animation clip count exceeds limit");
    }
    BinaryWriter writer;
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(value.clips.size())));
    for (const AssetGuid& clip : value.clips) {
        PARCEL_TRY(WriteGuid(writer, clip));
    }
    return writer.Release();
}

Result<AnimationPlayerComponent>
ComponentCodec<AnimationPlayerComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    PARCEL_TRY_DECL(std::uint32_t, count, ReadEntryCount(reader, 16, "animation clip"));
    AnimationPlayerComponent value;
    value.clips.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        PARCEL_TRY_DECL(AssetGuid, clip, ReadGuid(reader));
        value.clips.push_back(clip);
    }
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>> ComponentCodec<SkinComponent>::Encode(const SkinComponent& value) {
    if (value.joints.size() != value.inverse_bind_matrices.size()) {
        return MakeError(ErrorCode::InvalidArgument,
                         "skin joint and inverse bind matrix counts differ");
    }
    if (value.joints.size() > 65536) {
        return MakeError(ErrorCode::InvalidArgument, "skin joint count exceeds limit");
    }
    BinaryWriter writer;
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(value.joints.size())));
    for (std::size_t index = 0; index < value.joints.size(); ++index) {
        if (value.joints[index].IsRoot()) {
            return MakeError(ErrorCode::InvalidArgument, "skin joint entity is invalid");
        }
        for (float element : value.inverse_bind_matrices[index].m) {
            if (!std::isfinite(element)) {
                return MakeError(ErrorCode::InvalidArgument, "skin inverse bind matrix is invalid");
            }
        }
        PARCEL_TRY(writer.WriteU64(value.joints[index].value));
        PARCEL_TRY(serialize::WriteMat4(writer, value.inverse_bind_matrices[index]));
    }
    return writer.Release();
}

Result<SkinComponent> ComponentCodec<SkinComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    PARCEL_TRY_DECL(std::uint32_t, count, ReadEntryCount(reader, 72, "skin joint"));
    if (count == 0 || count > 65536) {
        return MakeError(ErrorCode::Corrupt, "skin component joint count is invalid");
    }
    SkinComponent value;
    value.joints.resize(count);
    value.inverse_bind_matrices.resize(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        PARCEL_TRY_ASSIGN(value.joints[index].value, reader.ReadU64());
        if (value.joints[index].IsRoot()) {
            return MakeError(ErrorCode::Corrupt, "skin joint entity is invalid");
        }
        PARCEL_TRY_ASSIGN(value.inverse_bind_matrices[index], serialize::ReadMat4(reader));
        for (float element : value.inverse_bind_matrices[index].m) {
            if (!std::isfinite(element)) {
                return MakeError(ErrorCode::Corrupt, "skin inverse bind matrix is invalid");
            }
        }
    }
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>> ComponentCodec<MorphComponent>::Encode(const MorphComponent& value) {
    if (value.weights.size() > kMaxMorphWeights) {
        return MakeError(ErrorCode::InvalidArgument, "morph weight count exceeds limit");
    }
    BinaryWriter writer;
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(value.weights.size())));
    for (float weight : value.weights) {
        if (!std::isfinite(weight)) {
            return MakeError(ErrorCode::InvalidArgument, "morph weight is invalid");
        }
        PARCEL_TRY(writer.WriteF32(weight));
    }
    return writer.Release();
}

Result<MorphComponent> ComponentCodec<MorphComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    PARCEL_TRY_DECL(std::uint32_t, count, ReadEntryCount(reader, 4, "morph weight"));
    if (count > kMaxMorphWeights) {
        return MakeError(ErrorCode::Corrupt, "morph weight count exceeds limit");
    }
    MorphComponent value;
    value.weights.resize(count);
    for (float& weight : value.weights) {
        PARCEL_TRY_ASSIGN(weight, reader.ReadF32());
        if (!std::isfinite(weight)) {
            return MakeError(ErrorCode::Corrupt, "morph weight is invalid");
        }
    }
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>> ComponentCodec<DecalComponent>::Encode(const DecalComponent& value) {
    BinaryWriter writer;
    PARCEL_TRY(WriteGuid(writer, value.albedo));
    PARCEL_TRY(WriteGuid(writer, value.normal));
    PARCEL_TRY(WriteGuid(writer, value.mask));
    PARCEL_TRY(serialize::WriteVec3(writer, value.position));
    PARCEL_TRY(serialize::WriteVec3(writer, value.rotation));
    PARCEL_TRY(serialize::WriteVec3(writer, value.size));
    PARCEL_TRY(serialize::WriteVec4(writer, value.base_color));
    PARCEL_TRY(writer.WriteF32(value.opacity));
    PARCEL_TRY(writer.WriteF32(value.angle_fade));
    PARCEL_TRY(writer.WriteF32(value.normal_blend));
    PARCEL_TRY(writer.WriteF32(value.metallic));
    PARCEL_TRY(writer.WriteF32(value.ambient_occlusion));
    PARCEL_TRY(writer.WriteF32(value.roughness));
    PARCEL_TRY(writer.WriteU32(value.layer_mask));
    PARCEL_TRY(writer.WriteU32(value.channel_mask));
    PARCEL_TRY(writer.WriteBool(value.enabled));
    return writer.Release();
}

Result<DecalComponent> ComponentCodec<DecalComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    DecalComponent value;
    PARCEL_TRY_ASSIGN(value.albedo, ReadGuid(reader));
    PARCEL_TRY_ASSIGN(value.normal, ReadGuid(reader));
    PARCEL_TRY_ASSIGN(value.mask, ReadGuid(reader));
    PARCEL_TRY_ASSIGN(value.position, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.rotation, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.size, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.base_color, serialize::ReadVec4(reader));
    PARCEL_TRY_ASSIGN(value.opacity, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.angle_fade, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.normal_blend, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.metallic, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.ambient_occlusion, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.roughness, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.layer_mask, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.channel_mask, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>>
ComponentCodec<ReflectionProbeComponent>::Encode(const ReflectionProbeComponent& value) {
    BinaryWriter writer;
    PARCEL_TRY(WriteGuid(writer, value.image));
    PARCEL_TRY(serialize::WriteVec3(writer, value.position));
    PARCEL_TRY(serialize::WriteVec3(writer, value.rotation));
    PARCEL_TRY(serialize::WriteVec3(writer, value.box_extents));
    PARCEL_TRY(writer.WriteF32(value.blend_distance));
    PARCEL_TRY(writer.WriteF32(value.intensity));
    PARCEL_TRY(writer.WriteBool(value.enabled));
    return writer.Release();
}

Result<ReflectionProbeComponent>
ComponentCodec<ReflectionProbeComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    ReflectionProbeComponent value;
    PARCEL_TRY_ASSIGN(value.image, ReadGuid(reader));
    PARCEL_TRY_ASSIGN(value.position, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.rotation, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.box_extents, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.blend_distance, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.intensity, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>>
ComponentCodec<LocalFogVolumeComponent>::Encode(const LocalFogVolumeComponent& value) {
    BinaryWriter writer;
    PARCEL_TRY(writer.WriteBool(value.enabled));
    PARCEL_TRY(WriteEnum(writer, value.shape));
    PARCEL_TRY(serialize::WriteVec3(writer, value.center));
    PARCEL_TRY(serialize::WriteVec3(writer, value.rotation));
    PARCEL_TRY(serialize::WriteVec3(writer, value.size));
    PARCEL_TRY(serialize::WriteVec3(writer, value.albedo));
    PARCEL_TRY(writer.WriteF32(value.mean_free_path));
    PARCEL_TRY(writer.WriteF32(value.anisotropy));
    PARCEL_TRY(serialize::WriteVec3(writer, value.positive_fade));
    PARCEL_TRY(serialize::WriteVec3(writer, value.negative_fade));
    PARCEL_TRY(writer.WriteBool(value.invert_fade));
    PARCEL_TRY(WriteEnum(writer, value.falloff));
    PARCEL_TRY(writer.WriteF32(value.distance_fade_start));
    PARCEL_TRY(writer.WriteF32(value.distance_fade_end));
    PARCEL_TRY(WriteGuid(writer, value.mask));
    PARCEL_TRY(serialize::WriteVec3(writer, value.mask_tiling));
    PARCEL_TRY(serialize::WriteVec3(writer, value.mask_scroll));
    PARCEL_TRY(WriteEnum(writer, value.mask_plane));
    PARCEL_TRY(writer.WriteF32(value.mask_intensity));
    PARCEL_TRY(writer.WriteBool(value.mask_invert));
    return writer.Release();
}

Result<LocalFogVolumeComponent>
ComponentCodec<LocalFogVolumeComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    LocalFogVolumeComponent value;
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.shape, ReadEnum<LocalFogShape>(reader, 1));
    PARCEL_TRY_ASSIGN(value.center, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.rotation, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.size, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.albedo, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.mean_free_path, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.anisotropy, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.positive_fade, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.negative_fade, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.invert_fade, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.falloff, ReadEnum<LocalFogFalloff>(reader, 1));
    PARCEL_TRY_ASSIGN(value.distance_fade_start, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.distance_fade_end, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.mask, ReadGuid(reader));
    PARCEL_TRY_ASSIGN(value.mask_tiling, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.mask_scroll, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.mask_plane, ReadEnum<LocalFogMaskPlane>(reader, 2));
    PARCEL_TRY_ASSIGN(value.mask_intensity, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.mask_invert, reader.ReadBool());
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>>
ComponentCodec<AudioSourceComponent>::Encode(const AudioSourceComponent& value) {
    BinaryWriter writer;
    PARCEL_TRY(WriteGuid(writer, value.clip));
    PARCEL_TRY(writer.WriteF32(value.volume));
    PARCEL_TRY(writer.WriteF32(value.pitch));
    PARCEL_TRY(writer.WriteBool(value.loop));
    PARCEL_TRY(writer.WriteBool(value.play_on_awake));
    PARCEL_TRY(writer.WriteBool(value.spatial));
    PARCEL_TRY(writer.WriteF32(value.min_distance));
    PARCEL_TRY(writer.WriteF32(value.max_distance));
    PARCEL_TRY(WriteEnum(writer, value.rolloff));
    return writer.Release();
}

Result<AudioSourceComponent>
ComponentCodec<AudioSourceComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    AudioSourceComponent value;
    PARCEL_TRY_ASSIGN(value.clip, ReadGuid(reader));
    PARCEL_TRY_ASSIGN(value.volume, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.pitch, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.loop, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.play_on_awake, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.spatial, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.min_distance, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.max_distance, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.rolloff, ReadEnum<AudioRolloffMode>(reader, 1));
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>>
ComponentCodec<AudioListenerComponent>::Encode(const AudioListenerComponent& value) {
    BinaryWriter writer;
    PARCEL_TRY(writer.WriteBool(value.enabled));
    return writer.Release();
}

Result<AudioListenerComponent>
ComponentCodec<AudioListenerComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    AudioListenerComponent value;
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>>
ComponentCodec<RigidBodyComponent>::Encode(const RigidBodyComponent& value) {
    BinaryWriter writer;
    PARCEL_TRY(WriteEnum(writer, value.motion_type));
    PARCEL_TRY(writer.WriteF32(value.mass));
    PARCEL_TRY(writer.WriteF32(value.drag));
    PARCEL_TRY(writer.WriteF32(value.angular_drag));
    PARCEL_TRY(writer.WriteBool(value.use_gravity));
    PARCEL_TRY(writer.WriteU32(value.constraints));
    PARCEL_TRY(WriteEnum(writer, value.interpolation));
    return writer.Release();
}

Result<RigidBodyComponent>
ComponentCodec<RigidBodyComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    RigidBodyComponent value;
    PARCEL_TRY_ASSIGN(value.motion_type, ReadEnum<MotionType>(reader, 2));
    PARCEL_TRY_ASSIGN(value.mass, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.drag, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.angular_drag, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.use_gravity, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.constraints, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.interpolation, ReadEnum<RigidBodyInterpolation>(reader, 2));
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>>
ComponentCodec<ColliderComponent>::Encode(const ColliderComponent& value) {
    BinaryWriter writer;
    PARCEL_TRY(WriteEnum(writer, value.shape));
    PARCEL_TRY(serialize::WriteVec3(writer, value.size));
    PARCEL_TRY(serialize::WriteVec3(writer, value.center));
    PARCEL_TRY(writer.WriteBool(value.trigger));
    PARCEL_TRY(writer.WriteF32(value.friction));
    PARCEL_TRY(writer.WriteF32(value.restitution));
    PARCEL_TRY(writer.WriteU32(value.layer));
    PARCEL_TRY(writer.WriteU32(value.mask));
    return writer.Release();
}

Result<ColliderComponent>
ComponentCodec<ColliderComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    ColliderComponent value;
    PARCEL_TRY_ASSIGN(value.shape, ReadEnum<ColliderShape>(reader, 3));
    PARCEL_TRY_ASSIGN(value.size, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.center, serialize::ReadVec3(reader));
    PARCEL_TRY_ASSIGN(value.trigger, reader.ReadBool());
    PARCEL_TRY_ASSIGN(value.friction, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.restitution, reader.ReadF32());
    PARCEL_TRY_ASSIGN(value.layer, reader.ReadU32());
    PARCEL_TRY_ASSIGN(value.mask, reader.ReadU32());
    PARCEL_TRY(Finish(reader));
    return value;
}

Result<std::vector<std::byte>>
ComponentCodec<ScriptComponent>::Encode(const ScriptComponent& value) {
    BinaryWriter writer;
    PARCEL_TRY(WriteEnum(writer, value.language));
    PARCEL_TRY(WriteGuid(writer, value.script));
    PARCEL_TRY(writer.WriteString(value.class_name));
    PARCEL_TRY(writer.WriteString(value.module_name));
    PARCEL_TRY(writer.WriteBool(value.enabled));
    if (value.properties.properties.size() > std::numeric_limits<std::uint32_t>::max()) {
        return MakeError(ErrorCode::InvalidArgument, "too many script properties");
    }
    PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(value.properties.properties.size())));
    for (const ScriptProperty& property : value.properties.properties) {
        if (property.name.empty()) {
            return MakeError(ErrorCode::InvalidArgument, "script property name is empty");
        }
        PARCEL_TRY(writer.WriteString(property.name));
        PARCEL_TRY(WriteEnum(writer, property.type));
        switch (property.type) {
        case PropertyType::Bool:
            PARCEL_TRY(writer.WriteBool(property.bool_value));
            break;
        case PropertyType::Int:
            PARCEL_TRY(writer.WriteI64(property.int_value));
            break;
        case PropertyType::Float:
            PARCEL_TRY(writer.WriteF32(property.float_value));
            break;
        case PropertyType::String:
            PARCEL_TRY(writer.WriteString(property.string_value));
            break;
        case PropertyType::Vec2:
            PARCEL_TRY(serialize::WriteVec2(writer, property.vec2_value));
            break;
        case PropertyType::Vec3:
            PARCEL_TRY(serialize::WriteVec3(writer, property.vec3_value));
            break;
        case PropertyType::Vec4:
            PARCEL_TRY(serialize::WriteVec4(writer, property.vec4_value));
            break;
        case PropertyType::Guid:
            PARCEL_TRY(WriteGuid(writer, property.guid_value));
            break;
        default:
            return MakeError(ErrorCode::InvalidArgument, "unknown script property type");
        }
    }
    return writer.Release();
}

Result<ScriptComponent>
ComponentCodec<ScriptComponent>::Decode(std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    ScriptComponent value;
    PARCEL_TRY_ASSIGN(value.language, ReadEnum<ScriptLanguage>(reader, 2));
    PARCEL_TRY_ASSIGN(value.script, ReadGuid(reader));
    PARCEL_TRY_DECL(std::string_view, class_name, reader.ReadString());
    value.class_name.assign(class_name);
    PARCEL_TRY_DECL(std::string_view, module_name, reader.ReadString());
    value.module_name.assign(module_name);
    PARCEL_TRY_ASSIGN(value.enabled, reader.ReadBool());
    PARCEL_TRY_DECL(std::uint32_t, property_count, ReadEntryCount(reader, 6, "script property"));
    value.properties.properties.reserve(property_count);
    for (std::uint32_t i = 0; i < property_count; ++i) {
        ScriptProperty property;
        PARCEL_TRY_DECL(std::string_view, name, reader.ReadString());
        if (name.empty()) {
            return MakeError(ErrorCode::Corrupt, "script property name is empty");
        }
        property.name.assign(name);
        PARCEL_TRY_ASSIGN(property.type, ReadEnum<PropertyType>(reader, 7));
        switch (property.type) {
        case PropertyType::Bool:
            PARCEL_TRY_ASSIGN(property.bool_value, reader.ReadBool());
            break;
        case PropertyType::Int:
            PARCEL_TRY_ASSIGN(property.int_value, reader.ReadI64());
            break;
        case PropertyType::Float:
            PARCEL_TRY_ASSIGN(property.float_value, reader.ReadF32());
            break;
        case PropertyType::String: {
            PARCEL_TRY_DECL(std::string_view, string_value, reader.ReadString());
            property.string_value.assign(string_value);
            break;
        }
        case PropertyType::Vec2:
            PARCEL_TRY_ASSIGN(property.vec2_value, serialize::ReadVec2(reader));
            break;
        case PropertyType::Vec3:
            PARCEL_TRY_ASSIGN(property.vec3_value, serialize::ReadVec3(reader));
            break;
        case PropertyType::Vec4:
            PARCEL_TRY_ASSIGN(property.vec4_value, serialize::ReadVec4(reader));
            break;
        case PropertyType::Guid:
            PARCEL_TRY_ASSIGN(property.guid_value, ReadGuid(reader));
            break;
        default:
            return MakeError(ErrorCode::Corrupt, "unknown script property type");
        }
        value.properties.properties.push_back(std::move(property));
    }
    PARCEL_TRY(Finish(reader));
    return value;
}

ComponentRegistry StandardRegistry() {
    ComponentRegistry registry;
    const auto add = [&registry](ComponentTypeInfo info) { registry.Register(info).Value(); };
    add({kTransformComponentType, std::string(ComponentCodec<TransformComponent>::type_name),
         kStandardComponentSchemaVersion, CollectTransform});
    add({kCameraComponentType, std::string(ComponentCodec<CameraComponent>::type_name),
         kStandardComponentSchemaVersion, CollectCamera});
    add({kLightComponentType, std::string(ComponentCodec<LightComponent>::type_name),
         kStandardComponentSchemaVersion, CollectLight});
    add({kMeshRendererComponentType, std::string(ComponentCodec<MeshRendererComponent>::type_name),
         kStandardComponentSchemaVersion, CollectMeshRenderer});
    add({kAnimationPlayerComponentType,
         std::string(ComponentCodec<AnimationPlayerComponent>::type_name),
         kStandardComponentSchemaVersion, CollectAnimationPlayer});
    add({kSkinComponentType, std::string(ComponentCodec<SkinComponent>::type_name),
         kStandardComponentSchemaVersion, CollectSkin});
    add({kMorphComponentType, std::string(ComponentCodec<MorphComponent>::type_name),
         kStandardComponentSchemaVersion, CollectMorph});
    add({kDecalComponentType, std::string(ComponentCodec<DecalComponent>::type_name),
         kStandardComponentSchemaVersion, CollectDecal});
    add({kReflectionProbeComponentType,
         std::string(ComponentCodec<ReflectionProbeComponent>::type_name),
         kStandardComponentSchemaVersion, CollectProbe});
    add({kLocalFogVolumeComponentType,
         std::string(ComponentCodec<LocalFogVolumeComponent>::type_name),
         kStandardComponentSchemaVersion, CollectFog});
    add({kAudioSourceComponentType, std::string(ComponentCodec<AudioSourceComponent>::type_name),
         kStandardComponentSchemaVersion, CollectAudio});
    add({kAudioListenerComponentType,
         std::string(ComponentCodec<AudioListenerComponent>::type_name),
         kStandardComponentSchemaVersion, CollectAudioListener});
    add({kRigidBodyComponentType, std::string(ComponentCodec<RigidBodyComponent>::type_name),
         kStandardComponentSchemaVersion, CollectRigidBody});
    add({kColliderComponentType, std::string(ComponentCodec<ColliderComponent>::type_name),
         kStandardComponentSchemaVersion, CollectCollider});
    add({kScriptComponentType, std::string(ComponentCodec<ScriptComponent>::type_name),
         kStandardComponentSchemaVersion, CollectScript});
    return registry;
}

const ComponentRegistry& StandardComponentRegistry() {
    static const ComponentRegistry registry = StandardRegistry();
    return registry;
}

} // namespace parcel::scene
