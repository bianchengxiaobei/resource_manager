#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/math.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace parcel::assets {

struct MeshVertex {
    Vec3 position{};
    Vec3 normal{0.0f, 0.0f, 1.0f};
    Vec4 tangent{0.0f, 0.0f, 0.0f, 1.0f};
    Vec2 texcoord0{};
    Vec2 texcoord1{};
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

    friend bool operator==(const MeshVertex&, const MeshVertex&) = default;
};

struct SkinVertex {
    std::array<std::uint16_t, 4> joints{};
    std::array<float, 4> weights{};

    friend bool operator==(const SkinVertex&, const SkinVertex&) = default;
};

inline constexpr std::uint32_t kMaxMorphTargets = 256;
inline constexpr std::uint64_t kMaxMorphDeltaVertices = 32ULL * 1000ULL * 1000ULL;

struct MorphTarget {
    std::string name;
    std::vector<Vec3> position_deltas;
    std::vector<Vec3> normal_deltas;
    std::vector<Vec4> tangent_deltas;
    bool has_normals = false;
    bool has_tangents = false;

    friend bool operator==(const MorphTarget&, const MorphTarget&) = default;
};

struct SubMesh {
    std::uint32_t index_offset = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material_index = 0;
    Bounds bounds;

    friend bool operator==(const SubMesh&, const SubMesh&) = default;
};

class MeshAsset final : public AssetObject {
  public:
    [[nodiscard]] TypeId Type() const noexcept override { return kMeshAssetType; }

    std::string name;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SkinVertex> skin_vertices;
    std::vector<MorphTarget> morph_targets;
    std::vector<float> morph_weights;
    std::vector<SubMesh> sub_meshes;
    Bounds bounds;
    bool has_normals = false;
    bool has_tangents = false;
    bool has_uv0 = false;
    bool has_uv1 = false;
    bool has_colors = false;
};

} // namespace parcel::assets
