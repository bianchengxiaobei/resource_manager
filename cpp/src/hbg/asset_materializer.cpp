#include "parcel/hbg/asset_materializer.hpp"

#include "parcel/core/error.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace parcel_hbg {
namespace {

using parcel::assets::AlphaMode;
using parcel::assets::MaterialFeature;
using parcel::assets::MaterialType;
using parcel::assets::TextureFormat;
using parcel::assets::TextureMip;
using parcel::assets::TextureRef;
using parcel::core::ErrorCode;
using parcel::core::MakeError;

hbg::Vec2 ToHbg(const parcel::Vec2& value) { return hbg::Vec2{value.x, value.y}; }

hbg::Vec3 ToHbg(const parcel::Vec3& value) { return hbg::Vec3{value.x, value.y, value.z}; }

hbg::Vec4 ToHbg(const parcel::Vec4& value) { return hbg::Vec4{value.x, value.y, value.z, value.w}; }

std::size_t BytesPerPixel(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8:
    case TextureFormat::BGRA8:
        return 4;
    case TextureFormat::RGBA16F:
        return 8;
    case TextureFormat::RGBA32F:
        return 16;
    case TextureFormat::BC1:
    case TextureFormat::BC3:
    case TextureFormat::BC5:
    case TextureFormat::BC7:
        return 0;
    }
    return 0;
}

std::size_t BlockBytes(TextureFormat format) {
    switch (format) {
    case TextureFormat::BC1:
        return 8;
    case TextureFormat::BC3:
    case TextureFormat::BC5:
    case TextureFormat::BC7:
        return 16;
    default:
        return 0;
    }
}

std::size_t TightRowBytes(const TextureMip& mip, TextureFormat format) {
    const std::size_t block_bytes = BlockBytes(format);
    if (block_bytes != 0) {
        return ((static_cast<std::size_t>(mip.width) + 3) / 4) * block_bytes;
    }
    return static_cast<std::size_t>(mip.width) * BytesPerPixel(format);
}

std::size_t RowCount(const TextureMip& mip, TextureFormat format) {
    if (BlockBytes(format) != 0) {
        return (static_cast<std::size_t>(mip.height) + 3) / 4;
    }
    return mip.height;
}

hbg::gfx::TextureFormat ToHbg(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8:
        return hbg::gfx::TextureFormat::RGBA8;
    case TextureFormat::BGRA8:
        return hbg::gfx::TextureFormat::BGRA8;
    case TextureFormat::RGBA16F:
        return hbg::gfx::TextureFormat::RGBA16F;
    case TextureFormat::RGBA32F:
        return hbg::gfx::TextureFormat::RGBA32F;
    case TextureFormat::BC1:
        return hbg::gfx::TextureFormat::BC1;
    case TextureFormat::BC3:
        return hbg::gfx::TextureFormat::BC3;
    case TextureFormat::BC5:
        return hbg::gfx::TextureFormat::BC5;
    case TextureFormat::BC7:
        return hbg::gfx::TextureFormat::BC7;
    }
    return hbg::gfx::TextureFormat::RGBA8;
}

Result<void> ValidateTextureFormat(TextureFormat format) {
    if (BytesPerPixel(format) == 0 && BlockBytes(format) == 0) {
        return MakeError(ErrorCode::Unsupported, "texture format is not supported");
    }
    return {};
}

Result<int> ResolveTexture(const TextureRef& ref, const ImageIndexResolver& resolve_image) {
    if (!ref.IsValid()) {
        return -1;
    }
    if (!resolve_image) {
        return MakeError(ErrorCode::InvalidArgument, "material image resolver is not set");
    }
    auto index = resolve_image(ref.texture);
    if (!index) {
        return std::move(index).TakeError();
    }
    if (index.Value() < 0) {
        return MakeError(ErrorCode::NotFound, "material texture was not resolved");
    }
    return index.Value();
}

hbg::asset::TextureWrap ToHbg(parcel::assets::TextureWrap wrap) {
    switch (wrap) {
    case parcel::assets::TextureWrap::Repeat:
        return hbg::asset::TextureWrap::Repeat;
    case parcel::assets::TextureWrap::Clamp:
        return hbg::asset::TextureWrap::Clamp;
    case parcel::assets::TextureWrap::Mirror:
        return hbg::asset::TextureWrap::Mirror;
    }
    return hbg::asset::TextureWrap::Clamp;
}

hbg::asset::TextureFilter ToHbg(parcel::assets::TextureFilter filter) {
    switch (filter) {
    case parcel::assets::TextureFilter::Linear:
        return hbg::asset::TextureFilter::Linear;
    case parcel::assets::TextureFilter::Nearest:
        return hbg::asset::TextureFilter::Nearest;
    }
    return hbg::asset::TextureFilter::Linear;
}

hbg::asset::TextureMipFilter ToHbg(parcel::assets::TextureMipFilter filter) {
    switch (filter) {
    case parcel::assets::TextureMipFilter::Linear:
        return hbg::asset::TextureMipFilter::Linear;
    case parcel::assets::TextureMipFilter::Nearest:
        return hbg::asset::TextureMipFilter::Nearest;
    }
    return hbg::asset::TextureMipFilter::Linear;
}

hbg::asset::TextureSampler ToHbg(const parcel::assets::TextureSampler& sampler) {
    hbg::asset::TextureSampler result;
    result.wrapS = ToHbg(sampler.wrap_s);
    result.wrapT = ToHbg(sampler.wrap_t);
    result.minFilter = ToHbg(sampler.min_filter);
    result.magFilter = ToHbg(sampler.mag_filter);
    result.mipFilter = ToHbg(sampler.mip_filter);
    return result;
}

hbg::asset::TextureRef ToHbg(const TextureRef& ref, int image_index) {
    return hbg::asset::TextureRef{image_index, static_cast<int>(ref.uv_set), ToHbg(ref.sampler)};
}

} // namespace

Result<MaterializedTexture> MaterializeTexture(const parcel::assets::TextureAsset& texture,
                                               std::string_view name) {
    PARCEL_TRY(ValidateTextureFormat(texture.format));
    if (texture.mips.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "texture has no mip levels");
    }
    if (texture.mip_count != 0 && texture.mips.size() != texture.mip_count) {
        return MakeError(ErrorCode::InvalidArgument, "texture mip count does not match mip array");
    }
    if (texture.mips.size() > std::numeric_limits<std::uint8_t>::max()) {
        return MakeError(ErrorCode::InvalidArgument, "texture has too many mip levels");
    }

    const TextureMip& base_mip = texture.mips.front();
    if (base_mip.width == 0 || base_mip.height == 0 ||
        base_mip.width > std::numeric_limits<std::uint16_t>::max() ||
        base_mip.height > std::numeric_limits<std::uint16_t>::max()) {
        return MakeError(ErrorCode::InvalidArgument, "texture dimensions are invalid");
    }

    auto storage = std::make_shared<std::vector<std::byte>>();
    storage->reserve(texture.mips.size() * 64);
    for (std::size_t mip_index = 0; mip_index < texture.mips.size(); ++mip_index) {
        const TextureMip& mip = texture.mips[mip_index];
        const std::uint32_t shift =
            static_cast<std::uint32_t>(std::min<std::size_t>(mip_index, 31));
        const std::uint32_t expected_width = std::max<std::uint32_t>(1, base_mip.width >> shift);
        const std::uint32_t expected_height = std::max<std::uint32_t>(1, base_mip.height >> shift);
        if (mip.width != expected_width || mip.height != expected_height) {
            return MakeError(ErrorCode::InvalidArgument, "texture mip dimensions are invalid");
        }

        const std::size_t tight_row = TightRowBytes(mip, texture.format);
        const std::size_t row_count = RowCount(mip, texture.format);
        const std::size_t row_pitch = mip.row_pitch == 0 ? tight_row : mip.row_pitch;
        if (row_pitch < tight_row ||
            row_count > std::numeric_limits<std::size_t>::max() / row_pitch ||
            mip.data.size() < row_count * row_pitch) {
            return MakeError(ErrorCode::InvalidArgument, "texture mip data is truncated");
        }

        const std::size_t offset = storage->size();
        storage->resize(offset + tight_row * row_count);
        for (std::size_t row = 0; row < row_count; ++row) {
            std::memcpy(storage->data() + offset + row * tight_row,
                        mip.data.data() + row * row_pitch, tight_row);
        }
    }

    MaterializedTexture result;
    result.desc.name = std::string(name);
    result.desc.width = static_cast<std::uint16_t>(base_mip.width);
    result.desc.height = static_cast<std::uint16_t>(base_mip.height);
    result.desc.layers = 1;
    result.desc.mipCount = static_cast<std::uint8_t>(texture.mips.size());
    result.desc.msaaSamples = 1;
    result.desc.format = ToHbg(texture.format);
    result.desc.flags =
        BGFX_SAMPLER_UVW_CLAMP | (texture.sRGB ? BGFX_TEXTURE_SRGB : BGFX_TEXTURE_NONE);
    result.desc.data = storage->data();
    result.desc.dataSize = storage->size() <= std::numeric_limits<std::uint32_t>::max()
                               ? static_cast<std::uint32_t>(storage->size())
                               : 0;
    if (result.desc.dataSize == 0) {
        return MakeError(ErrorCode::InvalidArgument, "texture data exceeds upload limits");
    }
    result.storage = std::move(storage);
    return result;
}

Result<hbg::asset::Mesh> MaterializeMesh(const parcel::assets::MeshAsset& mesh) {
    hbg::asset::Mesh result;
    result.name = mesh.name;
    result.vertices.reserve(mesh.vertices.size());
    for (const parcel::assets::MeshVertex& vertex : mesh.vertices) {
        hbg::asset::MeshVertex converted;
        converted.position = ToHbg(vertex.position);
        converted.normal = ToHbg(vertex.normal);
        converted.tangent = ToHbg(vertex.tangent);
        converted.texcoord0 = ToHbg(vertex.texcoord0);
        converted.texcoord1 = ToHbg(vertex.texcoord1);
        converted.color = ToHbg(vertex.color);
        result.vertices.push_back(converted);
    }
    result.indices = mesh.indices;
    if (!mesh.skin_vertices.empty()) {
        if (mesh.skin_vertices.size() != mesh.vertices.size()) {
            return MakeError(ErrorCode::InvalidArgument,
                             "mesh skin vertex count does not match vertex count");
        }
        result.skinVertices.resize(mesh.skin_vertices.size());
        result.skinInfluences.reserve(mesh.skin_vertices.size() * 4);
        for (std::size_t vertex = 0; vertex < mesh.skin_vertices.size(); ++vertex) {
            const parcel::assets::SkinVertex& source = mesh.skin_vertices[vertex];
            hbg::asset::SkinVertex skin;
            if (result.skinInfluences.size() > std::numeric_limits<std::uint32_t>::max()) {
                return MakeError(ErrorCode::InvalidArgument,
                                 "mesh skin influence count is too large");
            }
            skin.influenceOffset = static_cast<std::uint32_t>(result.skinInfluences.size());
            for (std::size_t influence = 0; influence < source.joints.size(); ++influence) {
                if (!std::isfinite(source.weights[influence]) || source.weights[influence] < 0.0f) {
                    return MakeError(ErrorCode::InvalidArgument, "mesh skin weight is invalid");
                }
                if (source.weights[influence] <= 0.0f) {
                    continue;
                }
                if (result.skinInfluences.size() >= std::numeric_limits<std::uint32_t>::max()) {
                    return MakeError(ErrorCode::InvalidArgument,
                                     "mesh skin influence count is too large");
                }
                result.skinInfluences.push_back(
                    hbg::asset::SkinInfluence{source.joints[influence], source.weights[influence]});
                ++skin.influenceCount;
            }
            result.skinVertices[vertex] = skin;
        }
    }
    if (mesh.morph_targets.size() > parcel::assets::kMaxMorphTargets ||
        static_cast<std::uint64_t>(mesh.morph_targets.size()) * mesh.vertices.size() >
            parcel::assets::kMaxMorphDeltaVertices) {
        return MakeError(ErrorCode::InvalidArgument, "mesh has too many morph targets");
    }
    if (!mesh.morph_weights.empty() && mesh.morph_weights.size() != mesh.morph_targets.size()) {
        return MakeError(ErrorCode::InvalidArgument,
                         "mesh morph weight count does not match morph target count");
    }
    result.morphWeights.reserve(mesh.morph_weights.size());
    for (float weight : mesh.morph_weights) {
        if (!std::isfinite(weight)) {
            return MakeError(ErrorCode::InvalidArgument, "mesh morph weight is invalid");
        }
        result.morphWeights.push_back(weight);
    }
    result.morphTargets.reserve(mesh.morph_targets.size());
    for (const parcel::assets::MorphTarget& source : mesh.morph_targets) {
        if (source.position_deltas.size() != mesh.vertices.size() ||
            (source.has_normals && source.normal_deltas.size() != mesh.vertices.size()) ||
            (source.has_tangents && source.tangent_deltas.size() != mesh.vertices.size()) ||
            (!source.has_normals && !source.normal_deltas.empty()) ||
            (!source.has_tangents && !source.tangent_deltas.empty())) {
            return MakeError(ErrorCode::InvalidArgument,
                             "mesh morph target delta count does not match vertex count");
        }
        hbg::asset::MorphTarget target;
        target.name = source.name;
        target.hasNormals = source.has_normals;
        target.hasTangents = source.has_tangents;
        target.positionDeltas.reserve(source.position_deltas.size());
        for (const parcel::Vec3& delta : source.position_deltas) {
            if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
                return MakeError(ErrorCode::InvalidArgument,
                                 "mesh morph position delta is invalid");
            }
            target.positionDeltas.push_back(ToHbg(delta));
        }
        target.normalDeltas.reserve(source.normal_deltas.size());
        for (const parcel::Vec3& delta : source.normal_deltas) {
            if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
                return MakeError(ErrorCode::InvalidArgument, "mesh morph normal delta is invalid");
            }
            target.normalDeltas.push_back(ToHbg(delta));
        }
        target.tangentDeltas.reserve(source.tangent_deltas.size());
        for (const parcel::Vec4& delta : source.tangent_deltas) {
            if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z) ||
                !std::isfinite(delta.w)) {
                return MakeError(ErrorCode::InvalidArgument, "mesh morph tangent delta is invalid");
            }
            target.tangentDeltas.push_back(ToHbg(delta));
        }
        result.morphTargets.push_back(std::move(target));
    }
    result.submeshes.reserve(mesh.sub_meshes.size());
    for (const parcel::assets::SubMesh& submesh : mesh.sub_meshes) {
        if (submesh.index_offset > mesh.indices.size() ||
            submesh.index_count > mesh.indices.size() - submesh.index_offset) {
            return MakeError(ErrorCode::InvalidArgument, "mesh submesh range is invalid");
        }
        hbg::asset::SubMesh converted;
        converted.indexOffset = submesh.index_offset;
        converted.indexCount = submesh.index_count;
        converted.materialIndex = submesh.material_index;
        converted.bounds.min =
            ToHbg(parcel::Vec3{submesh.bounds.center.x - submesh.bounds.extents.x,
                               submesh.bounds.center.y - submesh.bounds.extents.y,
                               submesh.bounds.center.z - submesh.bounds.extents.z});
        converted.bounds.max =
            ToHbg(parcel::Vec3{submesh.bounds.center.x + submesh.bounds.extents.x,
                               submesh.bounds.center.y + submesh.bounds.extents.y,
                               submesh.bounds.center.z + submesh.bounds.extents.z});
        result.submeshes.push_back(converted);
    }

    result.bounds.min = ToHbg(parcel::Vec3{mesh.bounds.center.x - mesh.bounds.extents.x,
                                           mesh.bounds.center.y - mesh.bounds.extents.y,
                                           mesh.bounds.center.z - mesh.bounds.extents.z});
    result.bounds.max = ToHbg(parcel::Vec3{mesh.bounds.center.x + mesh.bounds.extents.x,
                                           mesh.bounds.center.y + mesh.bounds.extents.y,
                                           mesh.bounds.center.z + mesh.bounds.extents.z});
    result.hasNormals = mesh.has_normals;
    result.hasTangents = mesh.has_tangents;
    result.hasUv0 = mesh.has_uv0;
    result.hasUv1 = mesh.has_uv1;
    result.hasColors = mesh.has_colors;
    return result;
}

Result<hbg::asset::Material> MaterializeMaterial(const parcel::assets::MaterialAsset& material,
                                                 const ImageIndexResolver& resolve_image) {
    hbg::asset::Material result;
    result.name = material.name;
    result.type = static_cast<hbg::asset::MaterialType>(material.type);
    result.alphaMode = static_cast<hbg::asset::AlphaMode>(material.alpha_mode);
    result.doubleSided = material.double_sided;
    result.features = material.features;
    result.renderQueue = material.render_queue;
    result.baseColorFactor = ToHbg(material.base_color_factor);
    result.metallic = material.metallic;
    result.roughness = material.roughness;
    result.emissiveFactor = ToHbg(material.emissive_factor);
    result.emissiveStrength = material.emissive_strength;
    result.normalScale = material.normal_scale;
    result.occlusionStrength = material.occlusion_strength;
    result.alphaCutoff = material.alpha_cutoff;

    PARCEL_TRY_DECL(int, base_color_image,
                    ResolveTexture(material.base_color_texture, resolve_image));
    PARCEL_TRY_DECL(int, metallic_roughness_image,
                    ResolveTexture(material.metallic_roughness_texture, resolve_image));
    PARCEL_TRY_DECL(int, normal_image, ResolveTexture(material.normal_texture, resolve_image));
    PARCEL_TRY_DECL(int, occlusion_image,
                    ResolveTexture(material.occlusion_texture, resolve_image));
    PARCEL_TRY_DECL(int, emissive_image, ResolveTexture(material.emissive_texture, resolve_image));

    result.baseColorTexture = ToHbg(material.base_color_texture, base_color_image);
    result.metallicRoughnessTexture =
        ToHbg(material.metallic_roughness_texture, metallic_roughness_image);
    result.normalTexture = ToHbg(material.normal_texture, normal_image);
    result.occlusionTexture = ToHbg(material.occlusion_texture, occlusion_image);
    result.emissiveTexture = ToHbg(material.emissive_texture, emissive_image);

    result.diffusionProfile = material.diffusion_profile;
    result.maskMapPacked = material.mask_map_packed;
    result.clearCoat = material.clear_coat;
    result.clearCoatRoughness = material.clear_coat_roughness;
    result.clearCoatNormalScale = material.clear_coat_normal_scale;
    result.transmission = material.transmission;
    result.thickness = material.thickness;
    result.ior = material.ior;
    result.iridescence = material.iridescence;
    result.iridescenceIor = material.iridescence_ior;
    result.iridescenceThickness = material.iridescence_thickness;
    result.anisotropy = material.anisotropy;
    result.anisotropyRotation = material.anisotropy_rotation;
    result.subsurfaceMask = material.subsurface_mask;
    return result;
}

hbg::asset::MaterialKey MakeMaterialKey(const hbg::asset::Material& material) {
    hbg::asset::MaterialKey key;
    key.type = static_cast<std::uint32_t>(material.type);
    key.alphaMode = static_cast<std::uint32_t>(material.alphaMode);
    key.features = material.features;
    key.doubleSided = material.doubleSided;
    key.renderQueue = material.renderQueue;
    key.baseColorFactor = material.baseColorFactor;
    key.metallic = material.metallic;
    key.roughness = material.roughness;
    key.emissiveFactor = material.emissiveFactor;
    key.emissiveStrength = material.emissiveStrength;
    key.normalScale = material.normalScale;
    key.occlusionStrength = material.occlusionStrength;
    key.alphaCutoff = material.alphaCutoff;
    key.clearCoat = material.clearCoat;
    key.clearCoatRoughness = material.clearCoatRoughness;
    key.clearCoatNormalScale = material.clearCoatNormalScale;
    key.transmission = material.transmission;
    key.thickness = material.thickness;
    key.ior = material.ior;
    key.iridescence = material.iridescence;
    key.iridescenceIor = material.iridescenceIor;
    key.iridescenceThickness = material.iridescenceThickness;
    key.anisotropy = material.anisotropy;
    key.anisotropyRotation = material.anisotropyRotation;
    key.subsurfaceMask = material.subsurfaceMask;
    key.baseColorImage = material.baseColorTexture.imageIndex;
    key.baseColorUv = material.baseColorTexture.uvSet;
    key.baseColorSampler = hbg::asset::samplerStateKey(material.baseColorTexture.sampler);
    key.metallicRoughnessImage = material.metallicRoughnessTexture.imageIndex;
    key.metallicRoughnessUv = material.metallicRoughnessTexture.uvSet;
    key.metallicRoughnessSampler =
        hbg::asset::samplerStateKey(material.metallicRoughnessTexture.sampler);
    key.normalImage = material.normalTexture.imageIndex;
    key.normalUv = material.normalTexture.uvSet;
    key.normalSampler = hbg::asset::samplerStateKey(material.normalTexture.sampler);
    key.occlusionImage = material.occlusionTexture.imageIndex;
    key.occlusionUv = material.occlusionTexture.uvSet;
    key.occlusionSampler = hbg::asset::samplerStateKey(material.occlusionTexture.sampler);
    key.emissiveImage = material.emissiveTexture.imageIndex;
    key.emissiveUv = material.emissiveTexture.uvSet;
    key.emissiveSampler = hbg::asset::samplerStateKey(material.emissiveTexture.sampler);
    return key;
}

Result<hbg::asset::MaterialKey> MakeMaterialKey(const parcel::assets::MaterialAsset& material,
                                                const ImageIndexResolver& resolve_image) {
    auto converted = MaterializeMaterial(material, resolve_image);
    if (!converted) {
        return std::move(converted).TakeError();
    }
    return MakeMaterialKey(converted.Value());
}

} // namespace parcel_hbg
