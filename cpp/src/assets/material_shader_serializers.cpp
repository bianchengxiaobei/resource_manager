#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/raw_file_asset.hpp"
#include "parcel/assets/serializers.hpp"
#include "parcel/assets/shader_library_asset.hpp"
#include "parcel/serialize/math_io.hpp"
#include "serializer_util.hpp"

#include <memory>

namespace parcel::assets::detail {
namespace {

constexpr std::uint32_t kMaxShaderEntries = 100U * 1000U;

Result<void> WriteTextureRef(BinaryWriter& writer, const TextureRef& value) {
    PARCEL_TRY(WriteGuid(writer, value.texture));
    PARCEL_TRY(writer.WriteI32(value.uv_set));
    PARCEL_TRY(WriteEnum(writer, value.sampler.wrap_s));
    PARCEL_TRY(WriteEnum(writer, value.sampler.wrap_t));
    PARCEL_TRY(WriteEnum(writer, value.sampler.min_filter));
    PARCEL_TRY(WriteEnum(writer, value.sampler.mag_filter));
    PARCEL_TRY(WriteEnum(writer, value.sampler.mip_filter));
    return {};
}

Result<TextureRef> ReadTextureRef(BinaryReader& reader) {
    TextureRef value;
    PARCEL_TRY_ASSIGN(value.texture, ReadGuid(reader));
    PARCEL_TRY_ASSIGN(value.uv_set, reader.ReadI32());
    PARCEL_TRY_ASSIGN(value.sampler.wrap_s, ReadEnum<TextureWrap>(reader, 2));
    PARCEL_TRY_ASSIGN(value.sampler.wrap_t, ReadEnum<TextureWrap>(reader, 2));
    PARCEL_TRY_ASSIGN(value.sampler.min_filter, ReadEnum<TextureFilter>(reader, 1));
    PARCEL_TRY_ASSIGN(value.sampler.mag_filter, ReadEnum<TextureFilter>(reader, 1));
    PARCEL_TRY_ASSIGN(value.sampler.mip_filter, ReadEnum<TextureMipFilter>(reader, 1));
    return value;
}

class MaterialAssetSerializer final : public ITypeSerializer {
  public:
    TypeId Type() const noexcept override { return kMaterialAssetType; }
    std::string_view TypeName() const noexcept override { return "MaterialAsset"; }
    std::uint32_t Version() const noexcept override { return 2; }

    Result<std::shared_ptr<AssetObject>> Create() const override {
        return std::static_pointer_cast<AssetObject>(std::make_shared<MaterialAsset>());
    }

    Result<void> Serialize(const AssetObject& object, BinaryWriter& writer,
                           IAssetWriteContext&) const override {
        const auto& asset = static_cast<const MaterialAsset&>(object);
        PARCEL_TRY(writer.WriteString(asset.name));
        PARCEL_TRY(WriteEnum(writer, asset.type));
        PARCEL_TRY(WriteEnum(writer, asset.alpha_mode));
        PARCEL_TRY(writer.WriteBool(asset.double_sided));
        PARCEL_TRY(writer.WriteU32(asset.features));
        PARCEL_TRY(writer.WriteI32(asset.render_queue));
        PARCEL_TRY(serialize::WriteVec4(writer, asset.base_color_factor));
        PARCEL_TRY(writer.WriteF32(asset.metallic));
        PARCEL_TRY(writer.WriteF32(asset.roughness));
        PARCEL_TRY(serialize::WriteVec3(writer, asset.emissive_factor));
        PARCEL_TRY(writer.WriteF32(asset.emissive_strength));
        PARCEL_TRY(writer.WriteF32(asset.normal_scale));
        PARCEL_TRY(writer.WriteF32(asset.occlusion_strength));
        PARCEL_TRY(writer.WriteF32(asset.alpha_cutoff));
        PARCEL_TRY(WriteTextureRef(writer, asset.base_color_texture));
        PARCEL_TRY(WriteTextureRef(writer, asset.metallic_roughness_texture));
        PARCEL_TRY(WriteTextureRef(writer, asset.normal_texture));
        PARCEL_TRY(WriteTextureRef(writer, asset.occlusion_texture));
        PARCEL_TRY(WriteTextureRef(writer, asset.emissive_texture));
        PARCEL_TRY(writer.WriteString(asset.diffusion_profile));
        PARCEL_TRY(writer.WriteBool(asset.mask_map_packed));
        PARCEL_TRY(writer.WriteF32(asset.clear_coat));
        PARCEL_TRY(writer.WriteF32(asset.clear_coat_roughness));
        PARCEL_TRY(writer.WriteF32(asset.clear_coat_normal_scale));
        PARCEL_TRY(writer.WriteF32(asset.transmission));
        PARCEL_TRY(writer.WriteF32(asset.thickness));
        PARCEL_TRY(writer.WriteF32(asset.ior));
        PARCEL_TRY(writer.WriteF32(asset.iridescence));
        PARCEL_TRY(writer.WriteF32(asset.iridescence_ior));
        PARCEL_TRY(writer.WriteF32(asset.iridescence_thickness));
        PARCEL_TRY(writer.WriteF32(asset.anisotropy));
        PARCEL_TRY(writer.WriteF32(asset.anisotropy_rotation));
        PARCEL_TRY(writer.WriteF32(asset.subsurface_mask));
        return {};
    }

    Result<std::shared_ptr<AssetObject>> Deserialize(BinaryReader& reader,
                                                     IAssetReadContext&) const override {
        auto asset = std::make_shared<MaterialAsset>();
        PARCEL_TRY_DECL(std::string_view, name, reader.ReadString());
        asset->name.assign(name);
        PARCEL_TRY_ASSIGN(asset->type, ReadEnum<MaterialType>(reader, 1));
        PARCEL_TRY_ASSIGN(asset->alpha_mode, ReadEnum<AlphaMode>(reader, 2));
        PARCEL_TRY_ASSIGN(asset->double_sided, reader.ReadBool());
        PARCEL_TRY_ASSIGN(asset->features, reader.ReadU32());
        PARCEL_TRY_ASSIGN(asset->render_queue, reader.ReadI32());
        PARCEL_TRY_ASSIGN(asset->base_color_factor, serialize::ReadVec4(reader));
        PARCEL_TRY_ASSIGN(asset->metallic, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->roughness, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->emissive_factor, serialize::ReadVec3(reader));
        PARCEL_TRY_ASSIGN(asset->emissive_strength, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->normal_scale, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->occlusion_strength, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->alpha_cutoff, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->base_color_texture, ReadTextureRef(reader));
        PARCEL_TRY_ASSIGN(asset->metallic_roughness_texture, ReadTextureRef(reader));
        PARCEL_TRY_ASSIGN(asset->normal_texture, ReadTextureRef(reader));
        PARCEL_TRY_ASSIGN(asset->occlusion_texture, ReadTextureRef(reader));
        PARCEL_TRY_ASSIGN(asset->emissive_texture, ReadTextureRef(reader));
        PARCEL_TRY_DECL(std::string_view, diffusion_profile, reader.ReadString());
        asset->diffusion_profile.assign(diffusion_profile);
        PARCEL_TRY_ASSIGN(asset->mask_map_packed, reader.ReadBool());
        PARCEL_TRY_ASSIGN(asset->clear_coat, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->clear_coat_roughness, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->clear_coat_normal_scale, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->transmission, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->thickness, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->ior, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->iridescence, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->iridescence_ior, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->iridescence_thickness, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->anisotropy, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->anisotropy_rotation, reader.ReadF32());
        PARCEL_TRY_ASSIGN(asset->subsurface_mask, reader.ReadF32());
        PARCEL_TRY(Finish(reader, "material metadata"));
        return std::static_pointer_cast<AssetObject>(asset);
    }

    Result<void> CollectReferences(const AssetObject& object,
                                   std::vector<AssetGuid>& out) const override {
        const auto& asset = static_cast<const MaterialAsset&>(object);
        const TextureRef* refs[] = {
            &asset.base_color_texture, &asset.metallic_roughness_texture, &asset.normal_texture,
            &asset.occlusion_texture,  &asset.emissive_texture,
        };
        for (const TextureRef* ref : refs) {
            if (!ref->texture.IsZero()) {
                out.push_back(ref->texture);
            }
        }
        return {};
    }
};

class ShaderLibraryAssetSerializer final : public ITypeSerializer {
  public:
    TypeId Type() const noexcept override { return kShaderLibraryAssetType; }
    std::string_view TypeName() const noexcept override { return "ShaderLibraryAsset"; }
    std::uint32_t Version() const noexcept override { return 1; }

    Result<std::shared_ptr<AssetObject>> Create() const override {
        return std::static_pointer_cast<AssetObject>(std::make_shared<ShaderLibraryAsset>());
    }

    Result<void> Serialize(const AssetObject& object, BinaryWriter& writer,
                           IAssetWriteContext& context) const override {
        const auto& asset = static_cast<const ShaderLibraryAsset&>(object);
        if (asset.backends.size() > kMaxShaderEntries) {
            return MakeError(ErrorCode::InvalidArgument, "shader has too many backends");
        }
        PARCEL_TRY(writer.WriteString(asset.name));
        PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(asset.backends.size())));
        for (const ShaderBackendLibrary& backend : asset.backends) {
            PARCEL_TRY(WriteEnum(writer, backend.backend));
            if (backend.passes.size() > kMaxShaderEntries ||
                backend.variants.size() > kMaxShaderEntries) {
                return MakeError(ErrorCode::InvalidArgument, "shader backend table is too large");
            }
            PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(backend.passes.size())));
            for (const ShaderPassBinary& pass : backend.passes) {
                PARCEL_TRY(writer.WriteString(pass.pass));
                PARCEL_TRY(WriteEnum(writer, pass.stage));
                std::uint64_t blob_id = pass.blob_id;
                if (!pass.data.empty()) {
                    PARCEL_TRY_ASSIGN(blob_id,
                                      WriteBlob(context, std::span<const std::byte>(pass.data)));
                }
                PARCEL_TRY(writer.WriteU64(blob_id));
            }
            PARCEL_TRY(writer.WriteU32(static_cast<std::uint32_t>(backend.variants.size())));
            for (const std::string& variant : backend.variants) {
                PARCEL_TRY(writer.WriteString(variant));
            }
        }
        PARCEL_TRY(writer.WriteString(asset.material_variant));
        PARCEL_TRY(writer.WriteString(asset.pipeline_key));
        return {};
    }

    Result<std::shared_ptr<AssetObject>> Deserialize(BinaryReader& reader,
                                                     IAssetReadContext& context) const override {
        auto asset = std::make_shared<ShaderLibraryAsset>();
        PARCEL_TRY_DECL(std::string_view, name, reader.ReadString());
        asset->name.assign(name);
        PARCEL_TRY_DECL(std::uint32_t, backend_count,
                        ReadCount(reader, kMaxShaderEntries, 5, "shader backend"));
        asset->backends.reserve(backend_count);
        for (std::uint32_t i = 0; i < backend_count; ++i) {
            ShaderBackendLibrary backend;
            PARCEL_TRY_ASSIGN(backend.backend, ReadEnum<ShaderBackend>(reader, 1));
            PARCEL_TRY_DECL(std::uint32_t, pass_count,
                            ReadCount(reader, kMaxShaderEntries, 14, "shader pass"));
            backend.passes.reserve(pass_count);
            for (std::uint32_t j = 0; j < pass_count; ++j) {
                ShaderPassBinary pass;
                PARCEL_TRY_DECL(std::string_view, pass_name, reader.ReadString());
                pass.pass.assign(pass_name);
                PARCEL_TRY_ASSIGN(pass.stage, ReadEnum<ShaderStage>(reader, 4));
                PARCEL_TRY_ASSIGN(pass.blob_id, reader.ReadU64());
                PARCEL_TRY_DECL(std::span<const std::byte>, data, ReadBlob(context, pass.blob_id));
                pass.data.assign(data.begin(), data.end());
                backend.passes.push_back(std::move(pass));
            }
            PARCEL_TRY_DECL(std::uint32_t, variant_count,
                            ReadCount(reader, kMaxShaderEntries, 4, "shader variant"));
            backend.variants.reserve(variant_count);
            for (std::uint32_t j = 0; j < variant_count; ++j) {
                PARCEL_TRY_DECL(std::string_view, variant, reader.ReadString());
                backend.variants.emplace_back(variant);
            }
            asset->backends.push_back(std::move(backend));
        }
        PARCEL_TRY_DECL(std::string_view, material_variant, reader.ReadString());
        asset->material_variant.assign(material_variant);
        PARCEL_TRY_DECL(std::string_view, pipeline_key, reader.ReadString());
        asset->pipeline_key.assign(pipeline_key);
        PARCEL_TRY(Finish(reader, "shader metadata"));
        return std::static_pointer_cast<AssetObject>(asset);
    }

    Result<void> CollectReferences(const AssetObject&, std::vector<AssetGuid>&) const override {
        return {};
    }
};

class RawFileAssetSerializer final : public ITypeSerializer {
  public:
    TypeId Type() const noexcept override { return kRawFileAssetType; }
    std::string_view TypeName() const noexcept override { return "RawFileAsset"; }
    std::uint32_t Version() const noexcept override { return 1; }

    Result<std::shared_ptr<AssetObject>> Create() const override {
        return std::static_pointer_cast<AssetObject>(std::make_shared<RawFileAsset>());
    }

    Result<void> Serialize(const AssetObject& object, BinaryWriter& writer,
                           IAssetWriteContext& context) const override {
        const auto& asset = static_cast<const RawFileAsset&>(object);
        PARCEL_TRY(writer.WriteString(asset.name));
        PARCEL_TRY_DECL(std::uint64_t, blob_id,
                        WriteBlob(context, std::span<const std::byte>(asset.data)));
        PARCEL_TRY(writer.WriteU64(blob_id));
        return {};
    }

    Result<std::shared_ptr<AssetObject>> Deserialize(BinaryReader& reader,
                                                     IAssetReadContext& context) const override {
        auto asset = std::make_shared<RawFileAsset>();
        PARCEL_TRY_DECL(std::string_view, name, reader.ReadString());
        asset->name.assign(name);
        PARCEL_TRY_DECL(std::uint64_t, blob_id, reader.ReadU64());
        PARCEL_TRY_DECL(std::span<const std::byte>, data, ReadBlob(context, blob_id));
        asset->data.assign(data.begin(), data.end());
        PARCEL_TRY(Finish(reader, "raw file metadata"));
        return std::static_pointer_cast<AssetObject>(asset);
    }

    Result<void> CollectReferences(const AssetObject&, std::vector<AssetGuid>&) const override {
        return {};
    }
};

class TextAssetSerializer final : public ITypeSerializer {
  public:
    TypeId Type() const noexcept override { return kTextAssetType; }
    std::string_view TypeName() const noexcept override { return "TextAsset"; }
    std::uint32_t Version() const noexcept override { return 1; }

    Result<std::shared_ptr<AssetObject>> Create() const override {
        return std::static_pointer_cast<AssetObject>(std::make_shared<TextAsset>());
    }

    Result<void> Serialize(const AssetObject& object, BinaryWriter& writer,
                           IAssetWriteContext&) const override {
        const auto& asset = static_cast<const TextAsset&>(object);
        PARCEL_TRY(writer.WriteString(asset.name));
        PARCEL_TRY(writer.WriteString(asset.text));
        return {};
    }

    Result<std::shared_ptr<AssetObject>> Deserialize(BinaryReader& reader,
                                                     IAssetReadContext&) const override {
        auto asset = std::make_shared<TextAsset>();
        PARCEL_TRY_DECL(std::string_view, name, reader.ReadString());
        asset->name.assign(name);
        PARCEL_TRY_DECL(std::string_view, text, reader.ReadString());
        asset->text.assign(text);
        PARCEL_TRY(Finish(reader, "text metadata"));
        return std::static_pointer_cast<AssetObject>(asset);
    }

    Result<void> CollectReferences(const AssetObject&, std::vector<AssetGuid>&) const override {
        return {};
    }
};

} // namespace

std::shared_ptr<const ITypeSerializer> MakeMaterialAssetSerializer() {
    return std::make_shared<MaterialAssetSerializer>();
}

std::shared_ptr<const ITypeSerializer> MakeShaderLibraryAssetSerializer() {
    return std::make_shared<ShaderLibraryAssetSerializer>();
}

std::shared_ptr<const ITypeSerializer> MakeRawFileAssetSerializer() {
    return std::make_shared<RawFileAssetSerializer>();
}

std::shared_ptr<const ITypeSerializer> MakeTextAssetSerializer() {
    return std::make_shared<TextAssetSerializer>();
}

} // namespace parcel::assets::detail
