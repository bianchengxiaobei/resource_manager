#pragma once

#include "parcel/assets/asset.hpp"

#include <string>
#include <vector>

namespace parcel::assets {

class RawFileAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kRawFileAssetType; }

    std::string name;
    std::vector<std::byte> data;
};

class TextAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kTextAssetType; }

    std::string name;
    std::string text;
};

} // namespace parcel::assets
