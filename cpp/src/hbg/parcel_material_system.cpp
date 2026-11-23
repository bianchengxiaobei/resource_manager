#include "parcel/hbg/parcel_material_system.hpp"

#include "parcel/core/error.hpp"

#include <shader/shader_key.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace parcel_hbg {
namespace {

using parcel::core::ErrorCode;
using parcel::core::MakeError;

hbg::gfx::VertexLayout MeshLayout() {
    hbg::gfx::VertexLayout layout;
    layout.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float);
    layout.add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float);
    layout.add(bgfx::Attrib::Tangent, 4, bgfx::AttribType::Float);
    layout.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float);
    layout.add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float);
    layout.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float);
    layout.end();
    return layout;
}

std::uint64_t SamplerFlags(const hbg::asset::TextureSampler& sampler) {
    std::uint64_t flags = BGFX_TEXTURE_NONE;
    switch (sampler.wrapS) {
    case hbg::asset::TextureWrap::Clamp:
        flags |= BGFX_SAMPLER_U_CLAMP;
        break;
    case hbg::asset::TextureWrap::Mirror:
        flags |= BGFX_SAMPLER_U_MIRROR;
        break;
    case hbg::asset::TextureWrap::Repeat:
    default:
        break;
    }
    switch (sampler.wrapT) {
    case hbg::asset::TextureWrap::Clamp:
        flags |= BGFX_SAMPLER_V_CLAMP;
        break;
    case hbg::asset::TextureWrap::Mirror:
        flags |= BGFX_SAMPLER_V_MIRROR;
        break;
    case hbg::asset::TextureWrap::Repeat:
    default:
        break;
    }
    if (sampler.minFilter == hbg::asset::TextureFilter::Nearest) {
        flags |= BGFX_SAMPLER_MIN_POINT;
    }
    if (sampler.magFilter == hbg::asset::TextureFilter::Nearest) {
        flags |= BGFX_SAMPLER_MAG_POINT;
    }
    if (sampler.mipFilter == hbg::asset::TextureMipFilter::Nearest) {
        flags |= BGFX_SAMPLER_MIP_POINT;
    }
    return flags;
}

} // namespace

bool ParcelMaterialSystem::CreateFallbackTextures(hbg::gfx::ResourceManager& resources) {
    const std::array<std::uint8_t, 4> white = {255, 255, 255, 255};
    const std::array<std::uint8_t, 4> normal = {128, 128, 255, 255};

    hbg::gfx::TextureDesc white_desc;
    white_desc.width = 1;
    white_desc.height = 1;
    white_desc.format = hbg::gfx::TextureFormat::RGBA8;
    white_desc.data = white.data();
    white_desc.dataSize = static_cast<std::uint32_t>(white.size());
    white_desc.flags = BGFX_SAMPLER_UVW_CLAMP | BGFX_TEXTURE_SRGB;
    white_desc.name = "parcel_white_srgb";
    white_srgb_ = resources.createTexture(white_desc);
    white_desc.flags = BGFX_SAMPLER_UVW_CLAMP;
    white_desc.name = "parcel_white_linear";
    white_linear_ = resources.createTexture(white_desc);

    hbg::gfx::TextureDesc normal_desc = white_desc;
    normal_desc.data = normal.data();
    normal_desc.name = "parcel_normal_flat";
    normal_flat_ = resources.createTexture(normal_desc);
    return white_srgb_.valid() && white_linear_.valid() && normal_flat_.valid();
}

hbg::gfx::TextureHandle ParcelMaterialSystem::TextureForImage(std::size_t image_index,
                                                              bool srgb_fallback) const {
    if (image_index < image_textures_.size() && image_textures_[image_index].valid()) {
        return image_textures_[image_index];
    }
    return srgb_fallback ? white_srgb_ : white_linear_;
}

Result<void> ParcelMaterialSystem::Initialize(hbg::gfx::ResourceManager& resources,
                                              const hbg::render::RenderScene& scene,
                                              std::span<const MaterializedTexture> images) {
    if (resources_ != nullptr) {
        return MakeError(ErrorCode::InvalidArgument, "ParcelMaterialSystem is already initialized");
    }
    if (images.size() != scene.images.size()) {
        return MakeError(ErrorCode::InvalidArgument,
                         "materialized image count does not match render scene");
    }

    resources_ = &resources;
    if (!samplers_.init(resources) || !CreateFallbackTextures(resources)) {
        Shutdown(resources);
        return MakeError(ErrorCode::Internal, "failed to create material samplers or fallbacks");
    }

    image_textures_.reserve(images.size());
    for (const MaterializedTexture& image : images) {
        hbg::gfx::TextureDesc desc = image.desc;
        const hbg::gfx::TextureHandle texture = resources.createTexture(desc);
        if (!texture.valid()) {
            Shutdown(resources);
            return MakeError(ErrorCode::Internal, "failed to upload material image");
        }
        image_textures_.push_back(texture);
    }

    const hbg::gfx::VertexLayout layout = MeshLayout();
    meshes_.reserve(scene.meshes.size());
    for (const hbg::asset::Mesh& mesh : scene.meshes) {
        hbg::render::GpuMesh gpu_mesh;
        if (!mesh.vertices.empty()) {
            hbg::gfx::BufferDesc desc;
            desc.type = hbg::gfx::BufferType::Vertex;
            desc.data = mesh.vertices.data();
            desc.size =
                static_cast<std::uint32_t>(mesh.vertices.size() * sizeof(hbg::asset::MeshVertex));
            desc.count = static_cast<std::uint32_t>(mesh.vertices.size());
            desc.layout = &layout;
            desc.name = mesh.name + ".vb";
            gpu_mesh.vertexBuffer = resources.createBuffer(desc);
        }
        if (!mesh.indices.empty()) {
            hbg::gfx::BufferDesc desc;
            desc.type = hbg::gfx::BufferType::Index;
            desc.data = mesh.indices.data();
            desc.size = static_cast<std::uint32_t>(mesh.indices.size() * sizeof(std::uint32_t));
            desc.count = static_cast<std::uint32_t>(mesh.indices.size());
            desc.flags = BGFX_BUFFER_INDEX32;
            desc.name = mesh.name + ".ib";
            gpu_mesh.indexBuffer = resources.createBuffer(desc);
        }
        if (!gpu_mesh.vertexBuffer.valid() || !gpu_mesh.indexBuffer.valid()) {
            Shutdown(resources);
            return MakeError(ErrorCode::Internal, "failed to create mesh GPU buffers");
        }
        meshes_.push_back(gpu_mesh);
    }

    materials_.reserve(scene.materials.size());
    for (const hbg::asset::Material& material : scene.materials) {
        hbg::render::GpuMaterial gpu_material;
        const auto texture = [&](const hbg::asset::TextureRef& ref, bool srgb) {
            if (!ref.valid() ||
                static_cast<std::size_t>(ref.imageIndex) >= image_textures_.size()) {
                return srgb ? white_srgb_ : white_linear_;
            }
            const std::size_t image_index = static_cast<std::size_t>(ref.imageIndex);
            const MaterializedTexture& image = images[image_index];
            const bool image_srgb = (image.desc.flags & BGFX_TEXTURE_SRGB) != 0;
            if (ref.sampler == hbg::asset::TextureSampler{} && image_srgb == srgb) {
                return TextureForImage(image_index, srgb);
            }
            const std::uint64_t key =
                (static_cast<std::uint64_t>(ref.imageIndex) << 32) |
                (static_cast<std::uint64_t>(hbg::asset::samplerStateKey(ref.sampler)) << 1) |
                (srgb ? 1ULL : 0ULL);
            if (const auto cached = material_textures_.find(key);
                cached != material_textures_.end()) {
                return cached->second;
            }
            hbg::gfx::TextureDesc desc = image.desc;
            desc.flags = SamplerFlags(ref.sampler) | (srgb ? BGFX_TEXTURE_SRGB : BGFX_TEXTURE_NONE);
            desc.name = desc.name.empty() ? "parcel_material_image" : desc.name + ".sampler";
            const hbg::gfx::TextureHandle handle = resources.createTexture(desc);
            material_textures_.emplace(key, handle);
            return handle;
        };
        gpu_material.baseColorTexture = texture(material.baseColorTexture, true);
        gpu_material.metallicRoughnessTexture = texture(material.metallicRoughnessTexture, false);
        gpu_material.normalTexture =
            material.normalTexture.valid() ? texture(material.normalTexture, false) : normal_flat_;
        gpu_material.occlusionTexture = texture(material.occlusionTexture, false);
        gpu_material.emissiveTexture = texture(material.emissiveTexture, true);
        if (!gpu_material.baseColorTexture.valid() ||
            !gpu_material.metallicRoughnessTexture.valid() || !gpu_material.normalTexture.valid() ||
            !gpu_material.occlusionTexture.valid() || !gpu_material.emissiveTexture.valid()) {
            Shutdown(resources);
            return MakeError(ErrorCode::Internal, "failed to upload material texture");
        }
        gpu_material.uniforms = MakeGpuMaterialUniforms(material);
        gpu_material.variantKey = MakeGpuMaterialVariant(material);
        gpu_material.features = material.features;
        materials_.push_back(gpu_material);
    }

    decals_.reserve(scene.decals.size());
    for (const hbg::render::Decal& decal : scene.decals) {
        hbg::render::GpuDecalTextures textures;
        if (decal.albedoImage >= 0 &&
            static_cast<std::size_t>(decal.albedoImage) < image_textures_.size()) {
            textures.albedo = image_textures_[static_cast<std::size_t>(decal.albedoImage)];
        }
        if (decal.normalImage >= 0 &&
            static_cast<std::size_t>(decal.normalImage) < image_textures_.size()) {
            textures.normal = image_textures_[static_cast<std::size_t>(decal.normalImage)];
        }
        if (decal.maskImage >= 0 &&
            static_cast<std::size_t>(decal.maskImage) < image_textures_.size()) {
            textures.mask = image_textures_[static_cast<std::size_t>(decal.maskImage)];
        }
        decals_.push_back(textures);
    }
    return {};
}

void ParcelMaterialSystem::Shutdown(hbg::gfx::ResourceManager& resources) {
    if (resources_ == nullptr) {
        return;
    }
    for (hbg::render::GpuMesh& mesh : meshes_) {
        if (mesh.vertexBuffer.valid()) {
            resources.destroyBuffer(mesh.vertexBuffer);
        }
        if (mesh.indexBuffer.valid()) {
            resources.destroyBuffer(mesh.indexBuffer);
        }
        if (mesh.wireframeIndexBuffer.valid()) {
            resources.destroyBuffer(mesh.wireframeIndexBuffer);
        }
    }
    meshes_.clear();
    materials_.clear();
    decals_.clear();

    for (const auto& entry : material_textures_) {
        if (entry.second.valid()) {
            resources.destroyTexture(entry.second);
        }
    }
    material_textures_.clear();
    for (const hbg::gfx::TextureHandle texture : image_textures_) {
        if (texture.valid()) {
            resources.destroyTexture(texture);
        }
    }
    image_textures_.clear();
    if (white_srgb_.valid()) {
        resources.destroyTexture(white_srgb_);
    }
    if (white_linear_.valid()) {
        resources.destroyTexture(white_linear_);
    }
    if (normal_flat_.valid()) {
        resources.destroyTexture(normal_flat_);
    }
    white_srgb_ = {};
    white_linear_ = {};
    normal_flat_ = {};
    samplers_.shutdown(resources);
    resources_ = nullptr;
}

} // namespace parcel_hbg
