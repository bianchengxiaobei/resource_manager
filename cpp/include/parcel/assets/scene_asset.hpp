#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/scene/scene_data.hpp"

#include <string>

namespace parcel::assets {

class SceneAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kSceneAssetType; }

    std::string name;
    scene::SceneData scene;
};

} // namespace parcel::assets
