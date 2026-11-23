#pragma once

#include "parcel/assets/asset.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace parcel::assets {

enum class ShaderBackend : std::uint8_t { Dxbc, Spirv };
enum class ShaderStage : std::uint8_t { Vertex, Fragment, BufferFragment, Compute, BufferCompute };

struct ShaderPassBinary {
    std::string pass;
    ShaderStage stage = ShaderStage::Vertex;
    std::uint64_t blob_id = 0;
    std::vector<std::byte> data;
};

struct ShaderBackendLibrary {
    ShaderBackend backend = ShaderBackend::Dxbc;
    std::vector<ShaderPassBinary> passes;
    std::vector<std::string> variants;
};

class ShaderLibraryAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kShaderLibraryAssetType; }

    std::string name;
    std::vector<ShaderBackendLibrary> backends;
    std::string material_variant;
    std::string pipeline_key;
};

} // namespace parcel::assets
