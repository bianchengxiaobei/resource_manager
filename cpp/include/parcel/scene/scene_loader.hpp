#pragma once

#include "parcel/core/result.hpp"
#include "parcel/scene/scene_data.hpp"

#include <memory>
#include <vector>

namespace parcel::scene {

struct SceneInstance {
    std::vector<SceneEntityId> entities;
    void* registry = nullptr;
    std::shared_ptr<void> context;
};

class ISceneLoader {
  public:
    virtual ~ISceneLoader() = default;

    [[nodiscard]] virtual Result<SceneInstance> Instantiate(const SceneData& scene,
                                                            void* registry) = 0;
    [[nodiscard]] virtual Result<void> Destroy(SceneInstance& instance) = 0;
};

} // namespace parcel::scene
