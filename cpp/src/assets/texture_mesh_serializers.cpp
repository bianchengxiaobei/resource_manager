#include "parcel/assets/mesh_asset.hpp"
#include "parcel/assets/serializers.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/serialize/math_io.hpp"
#include "serializer_util.hpp"

#include <cmath>
#include <cstring>
#include <memory>
#include <type_traits>

namespace parcel::assets::detail {
namespace {

constexpr std::uint32_t kMaxMeshEntries = 10U * 1000U * 1000U;
constexpr std::uint32_t kMaxTextureMips = 1024;

class TextureAssetSerializer final : public ITypeSerializer {
  public:
    TypeId Type() const noexcept override { return kTextureAssetType; }
    std::string_view TypeName() const noexcept override { return "TextureAsset"; }
    std::uint32_t Version() const noexcept override { return 1; }

    Result<std::shared_ptr<AssetObject>> Create() const override {
        return std::static_pointer_cast<AssetObject>(std::make_shared<TextureAsset>());
    }

    Result<void> Serialize(const AssetObject& object, BinaryWriter& writer,
                           IAssetWriteContext& context) const override {
        const auto& asset = static_cast<const TextureAsset&>(object);
        if (asset.mips.size() > std::numeric_limits<std::uint32_t>::max()) {
            return MakeError(ErrorCode::InvalidArgument, "texture has too many mips");
        }
        PARCEL_TRY(WriteEnum(writer, asset.format));
        PARCEL_TRY(writer.WriteBool(asset.sRGB));
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(asset.mips.size())));
        for (const TextureMip& mip : asset.mips) {
            PARCEL_TRY(writer.WriteU32(mip.width));
            PARCEL_TRY(writer.WriteU32(mip.height));
            PARCEL_TRY(writer.WriteU32(mip.row_pitch));
            PARCEL_TRY_DECL(
                std::uint64_t, blob_id,
                WriteBlob(context, std::as_bytes(std::span<const std::byte>(mip.data))));
            PARCEL_TRY(writer.WriteU64(blob_id));
        }
        return {};
    }

    Result<std::shared_ptr<AssetObject>> Deserialize(BinaryReader& reader,
                                                     IAssetReadContext& context) const override {
        auto asset = std::make_shared<TextureAsset>();
        PARCEL_TRY_ASSIGN(asset->format, ReadEnum<TextureFormat>(reader, 7));
        PARCEL_TRY_ASSIGN(asset->sRGB, reader.ReadBool());
        PARCEL_TRY_DECL(std::uint32_t, mip_count,
                        ReadCount(reader, kMaxTextureMips, 20, "texture mip"));
        asset->mips.reserve(mip_count);
        for (std::uint32_t i = 0; i < mip_count; ++i) {
            TextureMip mip;
            PARCEL_TRY_ASSIGN(mip.width, reader.ReadU32());
            PARCEL_TRY_ASSIGN(mip.height, reader.ReadU32());
            PARCEL_TRY_ASSIGN(mip.row_pitch, reader.ReadU32());
            PARCEL_TRY_DECL(std::uint64_t, blob_id, reader.ReadU64());
            PARCEL_TRY_DECL(std::span<const std::byte>, data, ReadBlob(context, blob_id));
            mip.data.assign(data.begin(), data.end());
            asset->mips.push_back(std::move(mip));
        }
        PARCEL_TRY(Finish(reader, "texture metadata"));
        asset->mip_count = static_cast<std::uint32_t>(asset->mips.size());
        return std::static_pointer_cast<AssetObject>(asset);
    }

    Result<void> CollectReferences(const AssetObject&, std::vector<AssetGuid>&) const override {
        return {};
    }
};

class MeshAssetSerializer final : public ITypeSerializer {
  public:
    TypeId Type() const noexcept override { return kMeshAssetType; }
    std::string_view TypeName() const noexcept override { return "MeshAsset"; }
    std::uint32_t Version() const noexcept override { return 3; }

    Result<std::shared_ptr<AssetObject>> Create() const override {
        return std::static_pointer_cast<AssetObject>(std::make_shared<MeshAsset>());
    }

    Result<void> Serialize(const AssetObject& object, BinaryWriter& writer,
                           IAssetWriteContext& context) const override {
        const auto& asset = static_cast<const MeshAsset&>(object);
        if (asset.vertices.size() > kMaxMeshEntries || asset.indices.size() > kMaxMeshEntries ||
            asset.sub_meshes.size() > kMaxMeshEntries) {
            return MakeError(ErrorCode::InvalidArgument, "mesh exceeds table limits");
        }
        PARCEL_TRY(writer.WriteString(asset.name));
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(asset.vertices.size())));
        PARCEL_TRY_DECL(
            std::uint64_t, vertex_blob,
            WriteBlob(context, std::as_bytes(std::span<const MeshVertex>(asset.vertices))));
        PARCEL_TRY(writer.WriteU64(vertex_blob));
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(asset.indices.size())));
        PARCEL_TRY_DECL(
            std::uint64_t, index_blob,
            WriteBlob(context, std::as_bytes(std::span<const std::uint32_t>(asset.indices))));
        PARCEL_TRY(writer.WriteU64(index_blob));
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(asset.sub_meshes.size())));
        for (const SubMesh& sub_mesh : asset.sub_meshes) {
            PARCEL_TRY(writer.WriteU32(sub_mesh.index_offset));
            PARCEL_TRY(writer.WriteU32(sub_mesh.index_count));
            PARCEL_TRY(writer.WriteU32(sub_mesh.material_index));
            PARCEL_TRY(serialize::WriteBounds(writer, sub_mesh.bounds));
        }
        PARCEL_TRY(serialize::WriteBounds(writer, asset.bounds));
        PARCEL_TRY(writer.WriteBool(asset.has_normals));
        PARCEL_TRY(writer.WriteBool(asset.has_tangents));
        PARCEL_TRY(writer.WriteBool(asset.has_uv0));
        PARCEL_TRY(writer.WriteBool(asset.has_uv1));
        PARCEL_TRY(writer.WriteBool(asset.has_colors));
        if (!asset.skin_vertices.empty() && asset.skin_vertices.size() != asset.vertices.size()) {
            return MakeError(ErrorCode::InvalidArgument,
                             "mesh skin vertex count does not match vertex count");
        }
        if (asset.skin_vertices.size() > kMaxMeshEntries) {
            return MakeError(ErrorCode::InvalidArgument, "mesh exceeds skin table limits");
        }
        for (const SkinVertex& skin : asset.skin_vertices) {
            for (float weight : skin.weights) {
                if (!std::isfinite(weight) || weight < 0.0f) {
                    return MakeError(ErrorCode::InvalidArgument, "mesh skin weight is invalid");
                }
            }
        }
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(asset.skin_vertices.size())));
        PARCEL_TRY_DECL(
            std::uint64_t, skin_blob,
            WriteBlob(context, std::as_bytes(std::span<const SkinVertex>(asset.skin_vertices))));
        PARCEL_TRY(writer.WriteU64(skin_blob));
        if (asset.morph_targets.size() > kMaxMorphTargets ||
            static_cast<std::uint64_t>(asset.morph_targets.size()) * asset.vertices.size() >
                kMaxMorphDeltaVertices) {
            return MakeError(ErrorCode::InvalidArgument, "mesh has too many morph targets");
        }
        if (!asset.morph_weights.empty() &&
            asset.morph_weights.size() != asset.morph_targets.size()) {
            return MakeError(ErrorCode::InvalidArgument,
                             "mesh morph weight count does not match morph target count");
        }
        for (float weight : asset.morph_weights) {
            if (!std::isfinite(weight)) {
                return MakeError(ErrorCode::InvalidArgument, "mesh morph weight is invalid");
            }
        }
        if (!asset.morph_targets.empty() && asset.vertices.empty()) {
            return MakeError(ErrorCode::InvalidArgument,
                             "mesh morph targets require mesh vertices");
        }
        for (const MorphTarget& target : asset.morph_targets) {
            if (target.position_deltas.size() != asset.vertices.size() ||
                (target.has_normals && target.normal_deltas.size() != asset.vertices.size()) ||
                (target.has_tangents && target.tangent_deltas.size() != asset.vertices.size()) ||
                (!target.has_normals && !target.normal_deltas.empty()) ||
                (!target.has_tangents && !target.tangent_deltas.empty())) {
                return MakeError(ErrorCode::InvalidArgument,
                                 "mesh morph target delta count does not match vertex count");
            }
            for (const Vec3& delta : target.position_deltas) {
                if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
                    return MakeError(ErrorCode::InvalidArgument,
                                     "mesh morph position delta is invalid");
                }
            }
            for (const Vec3& delta : target.normal_deltas) {
                if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
                    return MakeError(ErrorCode::InvalidArgument,
                                     "mesh morph normal delta is invalid");
                }
            }
            for (const Vec4& delta : target.tangent_deltas) {
                if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z) ||
                    !std::isfinite(delta.w)) {
                    return MakeError(ErrorCode::InvalidArgument,
                                     "mesh morph tangent delta is invalid");
                }
            }
        }
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(asset.morph_targets.size())));
        for (const MorphTarget& target : asset.morph_targets) {
            PARCEL_TRY(writer.WriteString(target.name));
            PARCEL_TRY(writer.WriteBool(target.has_normals));
            PARCEL_TRY(writer.WriteBool(target.has_tangents));
            PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(target.position_deltas.size())));
            PARCEL_TRY_DECL(
                std::uint64_t, position_blob,
                WriteBlob(context, std::as_bytes(std::span<const Vec3>(target.position_deltas))));
            PARCEL_TRY(writer.WriteU64(position_blob));
            if (target.has_normals) {
                PARCEL_TRY_DECL(
                    std::uint64_t, normal_blob,
                    WriteBlob(context, std::as_bytes(std::span<const Vec3>(target.normal_deltas))));
                PARCEL_TRY(writer.WriteU64(normal_blob));
            }
            if (target.has_tangents) {
                PARCEL_TRY_DECL(std::uint64_t, tangent_blob,
                                WriteBlob(context, std::as_bytes(std::span<const Vec4>(
                                                       target.tangent_deltas))));
                PARCEL_TRY(writer.WriteU64(tangent_blob));
            }
        }
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(asset.morph_weights.size())));
        for (float weight : asset.morph_weights) {
            PARCEL_TRY(writer.WriteF32(weight));
        }
        return {};
    }

    Result<std::shared_ptr<AssetObject>> Deserialize(BinaryReader& reader,
                                                     IAssetReadContext& context) const override {
        auto asset = std::make_shared<MeshAsset>();
        PARCEL_TRY_DECL(std::string_view, name, reader.ReadString());
        asset->name.assign(name);
        PARCEL_TRY_DECL(std::uint32_t, vertex_count,
                        ReadCount(reader, kMaxMeshEntries, 0, "vertex"));
        PARCEL_TRY_DECL(std::uint64_t, vertex_blob, reader.ReadU64());
        PARCEL_TRY_DECL(std::span<const std::byte>, vertex_data, ReadBlob(context, vertex_blob));
        if (vertex_data.size() != static_cast<std::uint64_t>(vertex_count) * sizeof(MeshVertex)) {
            return MakeError(ErrorCode::Corrupt, "mesh vertex blob size is invalid");
        }
        asset->vertices.resize(vertex_count);
        if (!vertex_data.empty()) {
            std::memcpy(asset->vertices.data(), vertex_data.data(), vertex_data.size());
        }
        PARCEL_TRY_DECL(std::uint32_t, index_count, ReadCount(reader, kMaxMeshEntries, 0, "index"));
        PARCEL_TRY_DECL(std::uint64_t, index_blob, reader.ReadU64());
        PARCEL_TRY_DECL(std::span<const std::byte>, index_data, ReadBlob(context, index_blob));
        if (index_data.size() != static_cast<std::uint64_t>(index_count) * sizeof(std::uint32_t)) {
            return MakeError(ErrorCode::Corrupt, "mesh index blob size is invalid");
        }
        asset->indices.resize(index_count);
        if (!index_data.empty()) {
            std::memcpy(asset->indices.data(), index_data.data(), index_data.size());
        }
        PARCEL_TRY_DECL(std::uint32_t, sub_mesh_count,
                        ReadCount(reader, kMaxMeshEntries, 28, "submesh"));
        asset->sub_meshes.reserve(sub_mesh_count);
        for (std::uint32_t i = 0; i < sub_mesh_count; ++i) {
            SubMesh sub_mesh;
            PARCEL_TRY_ASSIGN(sub_mesh.index_offset, reader.ReadU32());
            PARCEL_TRY_ASSIGN(sub_mesh.index_count, reader.ReadU32());
            PARCEL_TRY_ASSIGN(sub_mesh.material_index, reader.ReadU32());
            PARCEL_TRY_ASSIGN(sub_mesh.bounds, serialize::ReadBounds(reader));
            asset->sub_meshes.push_back(sub_mesh);
        }
        PARCEL_TRY_ASSIGN(asset->bounds, serialize::ReadBounds(reader));
        PARCEL_TRY_ASSIGN(asset->has_normals, reader.ReadBool());
        PARCEL_TRY_ASSIGN(asset->has_tangents, reader.ReadBool());
        PARCEL_TRY_ASSIGN(asset->has_uv0, reader.ReadBool());
        PARCEL_TRY_ASSIGN(asset->has_uv1, reader.ReadBool());
        PARCEL_TRY_ASSIGN(asset->has_colors, reader.ReadBool());
        PARCEL_TRY_DECL(std::uint32_t, skin_count,
                        ReadCount(reader, kMaxMeshEntries, 0, "skin vertex"));
        PARCEL_TRY_DECL(std::uint64_t, skin_blob, reader.ReadU64());
        PARCEL_TRY_DECL(std::span<const std::byte>, skin_data, ReadBlob(context, skin_blob));
        if (skin_data.size() != static_cast<std::uint64_t>(skin_count) * sizeof(SkinVertex)) {
            return MakeError(ErrorCode::Corrupt, "mesh skin vertex blob size is invalid");
        }
        if (skin_count != 0 && skin_count != asset->vertices.size()) {
            return MakeError(ErrorCode::Corrupt,
                             "mesh skin vertex count does not match vertex count");
        }
        asset->skin_vertices.resize(skin_count);
        if (!skin_data.empty()) {
            std::memcpy(asset->skin_vertices.data(), skin_data.data(), skin_data.size());
        }
        for (const SkinVertex& skin : asset->skin_vertices) {
            for (float weight : skin.weights) {
                if (!std::isfinite(weight) || weight < 0.0f) {
                    return MakeError(ErrorCode::Corrupt, "mesh skin weight is invalid");
                }
            }
        }
        PARCEL_TRY_DECL(std::uint32_t, morph_count,
                        ReadCount(reader, kMaxMorphTargets, 0, "morph target"));
        if (static_cast<std::uint64_t>(morph_count) * asset->vertices.size() >
            kMaxMorphDeltaVertices) {
            return MakeError(ErrorCode::Corrupt, "mesh has too many morph target deltas");
        }
        asset->morph_targets.reserve(morph_count);
        for (std::uint32_t target_index = 0; target_index < morph_count; ++target_index) {
            MorphTarget target;
            PARCEL_TRY_DECL(std::string_view, target_name, reader.ReadString());
            target.name.assign(target_name);
            PARCEL_TRY_ASSIGN(target.has_normals, reader.ReadBool());
            PARCEL_TRY_ASSIGN(target.has_tangents, reader.ReadBool());
            PARCEL_TRY_DECL(std::uint32_t, delta_count,
                            ReadCount(reader, kMaxMeshEntries, 0, "morph vertex"));
            if (delta_count != asset->vertices.size()) {
                return MakeError(ErrorCode::Corrupt,
                                 "mesh morph target delta count does not match vertex count");
            }
            PARCEL_TRY_DECL(std::uint64_t, position_blob, reader.ReadU64());
            PARCEL_TRY_DECL(std::span<const std::byte>, position_data,
                            ReadBlob(context, position_blob));
            if (position_data.size() != static_cast<std::uint64_t>(delta_count) * sizeof(Vec3)) {
                return MakeError(ErrorCode::Corrupt,
                                 "mesh morph position delta blob size is invalid");
            }
            target.position_deltas.resize(delta_count);
            if (!position_data.empty()) {
                std::memcpy(target.position_deltas.data(), position_data.data(),
                            position_data.size());
            }
            if (target.has_normals) {
                PARCEL_TRY_DECL(std::uint64_t, normal_blob, reader.ReadU64());
                PARCEL_TRY_DECL(std::span<const std::byte>, normal_data,
                                ReadBlob(context, normal_blob));
                if (normal_data.size() != static_cast<std::uint64_t>(delta_count) * sizeof(Vec3)) {
                    return MakeError(ErrorCode::Corrupt,
                                     "mesh morph normal delta blob size is invalid");
                }
                target.normal_deltas.resize(delta_count);
                if (!normal_data.empty()) {
                    std::memcpy(target.normal_deltas.data(), normal_data.data(),
                                normal_data.size());
                }
            }
            if (target.has_tangents) {
                PARCEL_TRY_DECL(std::uint64_t, tangent_blob, reader.ReadU64());
                PARCEL_TRY_DECL(std::span<const std::byte>, tangent_data,
                                ReadBlob(context, tangent_blob));
                if (tangent_data.size() != static_cast<std::uint64_t>(delta_count) * sizeof(Vec4)) {
                    return MakeError(ErrorCode::Corrupt,
                                     "mesh morph tangent delta blob size is invalid");
                }
                target.tangent_deltas.resize(delta_count);
                if (!tangent_data.empty()) {
                    std::memcpy(target.tangent_deltas.data(), tangent_data.data(),
                                tangent_data.size());
                }
            }
            for (const Vec3& delta : target.position_deltas) {
                if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
                    return MakeError(ErrorCode::Corrupt, "mesh morph position delta is invalid");
                }
            }
            for (const Vec3& delta : target.normal_deltas) {
                if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
                    return MakeError(ErrorCode::Corrupt, "mesh morph normal delta is invalid");
                }
            }
            for (const Vec4& delta : target.tangent_deltas) {
                if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z) ||
                    !std::isfinite(delta.w)) {
                    return MakeError(ErrorCode::Corrupt, "mesh morph tangent delta is invalid");
                }
            }
            asset->morph_targets.push_back(std::move(target));
        }
        PARCEL_TRY_DECL(std::uint32_t, morph_weight_count,
                        ReadCount(reader, kMaxMorphTargets, 0, "morph weight"));
        if (morph_weight_count != 0 && morph_weight_count != asset->morph_targets.size()) {
            return MakeError(ErrorCode::Corrupt,
                             "mesh morph weight count does not match morph target count");
        }
        asset->morph_weights.resize(morph_weight_count);
        for (float& weight : asset->morph_weights) {
            PARCEL_TRY_ASSIGN(weight, reader.ReadF32());
            if (!std::isfinite(weight)) {
                return MakeError(ErrorCode::Corrupt, "mesh morph weight is invalid");
            }
        }
        PARCEL_TRY(Finish(reader, "mesh metadata"));
        return std::static_pointer_cast<AssetObject>(asset);
    }

    Result<void> CollectReferences(const AssetObject&, std::vector<AssetGuid>&) const override {
        return {};
    }
};

static_assert(std::is_trivially_copyable_v<MeshVertex>);
static_assert(std::is_trivially_copyable_v<SkinVertex>);
static_assert(std::is_trivially_copyable_v<Vec3>);
static_assert(std::is_trivially_copyable_v<Vec4>);

} // namespace

std::shared_ptr<const ITypeSerializer> MakeTextureAssetSerializer() {
    return std::make_shared<TextureAssetSerializer>();
}

std::shared_ptr<const ITypeSerializer> MakeMeshAssetSerializer() {
    return std::make_shared<MeshAssetSerializer>();
}

} // namespace parcel::assets::detail
