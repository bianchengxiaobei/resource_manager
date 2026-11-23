#pragma once

#include "parcel/assets/asset.hpp"

#include <cstdint>
#include <vector>

namespace parcel::assets {

enum class TextureFormat : std::uint8_t {
    RGBA8,
    BGRA8,
    RGBA16F,
    RGBA32F,
    BC1,
    BC3,
    BC5,
    BC7,
};

struct TextureMip {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_pitch = 0;
    std::vector<std::byte> data;
};

class TextureAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kTextureAssetType; }

    TextureFormat format = TextureFormat::RGBA8;
    bool sRGB = false;
    std::uint32_t mip_count = 0;
    std::vector<TextureMip> mips;
};

} // namespace parcel::assets
