#include "parcel/hbg/parcel_material_system.hpp"

namespace parcel_hbg {

hbg::gfx::generated::MaterialUniforms
MakeGpuMaterialUniforms(const hbg::asset::Material& material) {
    hbg::gfx::generated::MaterialUniforms uniforms;
    uniforms.baseColorFactor = material.baseColorFactor;
    uniforms.metallicRoughness = hbg::Vec4{material.metallic, material.roughness,
                                           material.normalScale, material.alphaCutoff};
    uniforms.emissiveFactor = hbg::Vec4{material.emissiveFactor.x * material.emissiveStrength,
                                        material.emissiveFactor.y * material.emissiveStrength,
                                        material.emissiveFactor.z * material.emissiveStrength,
                                        material.occlusionStrength};
    uniforms.advanced0 = hbg::Vec4{material.clearCoat, material.clearCoatRoughness,
                                   material.transmission, material.thickness};
    uniforms.advanced1 = hbg::Vec4{material.anisotropy, material.anisotropyRotation, material.ior,
                                   material.iridescence};
    uniforms.advanced2 =
        hbg::Vec4{material.iridescenceThickness, material.subsurfaceMask,
                  static_cast<float>(material.features), material.clearCoatNormalScale};
    return uniforms;
}

std::uint64_t MakeGpuMaterialVariant(const hbg::asset::Material& material) {
    std::uint64_t variant = hbg::shader::MaterialVariantNone;
    if (material.normalTexture.valid()) {
        variant |= hbg::shader::MaterialVariantNormalMap;
    }
    if (material.metallicRoughnessTexture.valid() || material.occlusionTexture.valid()) {
        variant |= hbg::shader::MaterialVariantMaskMap;
    }
    if (material.alphaMode == hbg::asset::AlphaMode::Mask) {
        variant |= hbg::shader::MaterialVariantAlphaClip;
    }
    if (material.doubleSided) {
        variant |= hbg::shader::MaterialVariantDoubleSided;
    }
    if (material.alphaMode == hbg::asset::AlphaMode::Blend) {
        variant |= hbg::shader::MaterialVariantTransparent;
    }
    return variant;
}

} // namespace parcel_hbg
