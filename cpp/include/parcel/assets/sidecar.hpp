#pragma once

#include "parcel/assets/json.hpp"
#include "parcel/core/result.hpp"
#include "parcel/math.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::assets {

enum class SidecarProjection : std::uint8_t { Perspective, Orthographic };

struct SidecarSettings {
    std::string pipeline;
    std::string frame;
    std::string post_process;
    std::string shadows;
    std::string volumetric_fog;
};

struct SidecarCamera {
    std::string name;
    SidecarProjection projection = SidecarProjection::Perspective;
    float yfov = 1.0471975512f;
    float aspect = 1.7777778f;
    float near_clip = 0.1f;
    float far_clip = 1000.0f;
    float xmag = 1.0f;
    float ymag = 1.0f;
    Vec4 clear_color{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t culling_mask = 0xffffffffU;
    std::int32_t priority = 0;
};

struct SidecarObject {
    std::string name;
    JsonValue data;
};

struct SceneSidecar {
    std::uint32_t version = 1;
    std::string source;
    std::string coordinate_system = "rh-y-up-ccw";
    SidecarSettings settings;
    std::vector<SidecarCamera> cameras;
    std::vector<SidecarObject> lights;
    std::vector<SidecarObject> decals;
    std::vector<SidecarObject> reflection_probes;
    std::vector<SidecarObject> local_fog_volumes;
};

[[nodiscard]] core::Result<SceneSidecar> ParseSceneSidecar(std::string_view text);
[[nodiscard]] core::Result<SceneSidecar> LoadSceneSidecarFile(std::string_view path);

} // namespace parcel::assets
