#pragma once

#include "parcel/assets/asset.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace parcel::assets {

inline constexpr std::uint32_t kMaxIesAngles = 512;

class IesProfileAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kIesProfileAssetType; }

    std::string name;
    std::vector<float> vertical_angles;
    std::vector<float> horizontal_angles;
    std::vector<float> values;
};

[[nodiscard]] Result<void> ValidateIesProfile(const IesProfileAsset& profile);

} // namespace parcel::assets
