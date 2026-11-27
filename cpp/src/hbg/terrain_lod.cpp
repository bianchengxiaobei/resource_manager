#include "parcel/hbg/terrain_lod.hpp"

#include "parcel/assets/json.hpp"
#include "parcel/hbg/heightmap_tile_cache.hpp"

#include "core/bounds.h"
#include "render/culling/frustum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace parcel_hbg {
namespace {

constexpr float kSplitDistanceMin = 1.1f;
constexpr float kReferenceFov = 1.3962634f;
constexpr float kReferenceViewportHeight = 1024.0f;
constexpr int kHorizonSize = 256;

struct QuadSettings {
    float split_factor = 2.0f;
    int max_level = 4;
    float viewport_height = 1080.0f;
    float fov_y_radians = 0.9599311f;
    bool split_invisible_quads = false;
    bool horizon_culling = true;
};

struct QuadNode {
    QuadNode* parent = nullptr;
    int level = 0;
    int tx = 0;
    int ty = 0;
    float ox = 0.0f;
    float oy = 0.0f;
    float size = 0.0f;
    float zmin = 0.0f;
    float zmax = 0.0f;
    bool visible = true;
    bool occluded = false;
    bool horizon_culled = false;
    std::array<std::uint8_t, 4> child_order{0, 1, 2, 3};
    std::array<std::unique_ptr<QuadNode>, 4> children;

    bool is_leaf() const { return children[0] == nullptr; }
    float xmax() const { return ox + size; }
    float ymax() const { return oy + size; }
};

struct QuadStats {
    std::uint32_t node_count = 0;
    std::uint32_t leaf_count = 0;
    std::uint32_t visible_leaf_count = 0;
    std::uint32_t horizon_culled_leaf_count = 0;
    std::uint32_t max_depth = 0;
    bool horizon_active = false;
};

std::uint64_t NodeKey(int level, int tx, int ty) {
    return (static_cast<std::uint64_t>(level) << 32) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tx)) << 16) |
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(ty));
}

bool IntersectsFrustum(const std::array<hbg::Vec4, 6>& planes, const hbg::Bounds& bounds) {
    if (!bounds.valid()) {
        return true;
    }
    const hbg::Vec3 center = bounds.center();
    const hbg::Vec3 extents = bounds.extents();
    for (const hbg::Vec4& plane : planes) {
        const float distance =
            plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
        const float radius = std::abs(plane.x) * extents.x + std::abs(plane.y) * extents.y +
                             std::abs(plane.z) * extents.z;
        if (distance + radius < 0.0f) {
            return false;
        }
    }
    return true;
}

hbg::Bounds NodeBounds(const QuadNode& node) {
    hbg::Bounds bounds;
    bounds.min = hbg::Vec3(node.ox, node.zmin, node.oy);
    bounds.max = hbg::Vec3(node.xmax(), node.zmax, node.ymax());
    return bounds;
}

class QuadTree {
  public:
    void init(float ox, float oy, float size, float zmin, float zmax,
              const QuadSettings& settings) {
        settings_ = settings;
        settings_.max_level = std::max(0, settings_.max_level);
        settings_.split_factor = std::max(0.0f, settings_.split_factor);
        split_distance_ = ComputeSplitDistance(settings_);

        root_ = std::make_unique<QuadNode>();
        root_->level = 0;
        root_->tx = 0;
        root_->ty = 0;
        root_->ox = ox;
        root_->oy = oy;
        root_->size = size;
        root_->zmin = zmin;
        root_->zmax = zmax;
        ApplyHeightRange(*root_);
        stats_ = {};
    }

    void clear_node_height_ranges() { height_ranges_.clear(); }

    void set_height_range_provider(std::function<bool(int, int, int, float&, float&)> provider) {
        height_provider_ = std::move(provider);
    }

    void set_node_height_range(int level, int tx, int ty, float zmin, float zmax) {
        if (zmin > zmax) {
            std::swap(zmin, zmax);
        }
        height_ranges_[NodeKey(level, tx, ty)] = {zmin, zmax};
        if (root_ != nullptr && root_->level == level && root_->tx == tx && root_->ty == ty) {
            root_->zmin = zmin;
            root_->zmax = zmax;
        }
    }

    void update(const hbg::Vec3& camera_position, float ground_height,
                const std::array<hbg::Vec4, 6>* frustum_planes, const hbg::Vec3* camera_forward) {
        if (root_ == nullptr) {
            return;
        }
        BeginHorizon(camera_position, camera_forward);
        UpdateNode(*root_, camera_position, ground_height, frustum_planes);
        stats_ = {};
        stats_.horizon_active = horizon_active_;
        Count(*root_);
    }

    void collect_leaves(std::vector<const QuadNode*>& leaves) const {
        leaves.clear();
        if (root_ != nullptr) {
            CollectLeavesRecursive(*root_, leaves);
        }
    }

    const QuadStats& stats() const { return stats_; }

    static float ComputeSplitDistance(const QuadSettings& settings) {
        const float fov = std::max(settings.fov_y_radians, 0.0001f);
        const float viewport_height = std::max(settings.viewport_height, 1.0f);
        float distance = settings.split_factor * (viewport_height / kReferenceViewportHeight) *
                         std::tan(kReferenceFov * 0.5f) / std::tan(fov * 0.5f);
        if (!std::isfinite(distance) || distance < kSplitDistanceMin) {
            distance = kSplitDistanceMin;
        }
        return distance;
    }

  private:
    void ApplyHeightRange(QuadNode& node) const {
        if (height_provider_) {
            float zmin = node.zmin;
            float zmax = node.zmax;
            if (height_provider_(node.level, node.tx, node.ty, zmin, zmax)) {
                if (zmin > zmax) {
                    std::swap(zmin, zmax);
                }
                node.zmin = zmin;
                node.zmax = zmax;
                return;
            }
        }
        const auto found = height_ranges_.find(NodeKey(node.level, node.tx, node.ty));
        if (found != height_ranges_.end()) {
            node.zmin = found->second.first;
            node.zmax = found->second.second;
        }
    }

    static float DistanceToQuad(const QuadNode& node, const hbg::Vec3& camera_position,
                                float ground_height) {
        const float zmax = std::max(0.0f, ground_height);
        const float dy = std::abs(camera_position.y - zmax);
        const float dx = std::min(std::abs(camera_position.x - node.ox),
                                  std::abs(camera_position.x - node.xmax()));
        const float dz = std::min(std::abs(camera_position.z - node.oy),
                                  std::abs(camera_position.z - node.ymax()));
        return std::max(dy, std::max(dx, dz));
    }

    void UpdateNode(QuadNode& node, const hbg::Vec3& camera_position, float ground_height,
                    const std::array<hbg::Vec4, 6>* frustum_planes) {
        node.horizon_culled = false;
        if (!horizon_active_) {
            node.occluded = false;
        }
        ApplyHeightRange(node);
        node.visible =
            frustum_planes == nullptr || IntersectsFrustum(*frustum_planes, NodeBounds(node));

        if (!node.visible && !settings_.split_invisible_quads) {
            for (auto& child : node.children) {
                child.reset();
            }
            return;
        }

        if (horizon_active_ && node.occluded) {
            node.occluded = IsOccluded(node, camera_position);
            if (node.occluded) {
                node.visible = false;
                node.horizon_culled = true;
            }
        }

        const float distance = DistanceToQuad(node, camera_position, ground_height);
        const bool should_split = node.level < settings_.max_level &&
                                  distance < node.size * split_distance_ &&
                                  (settings_.split_invisible_quads || node.visible);

        if (should_split) {
            if (node.is_leaf()) {
                Subdivide(node);
            }
            SetChildOrder(node, camera_position);
            for (const std::uint8_t order : node.child_order) {
                UpdateNode(*node.children[order], camera_position, ground_height, frustum_planes);
            }
            node.occluded = node.children[0]->occluded && node.children[1]->occluded &&
                            node.children[2]->occluded && node.children[3]->occluded;
        } else {
            if (node.visible) {
                node.occluded = AddOccluder(node, camera_position);
                if (node.occluded) {
                    node.visible = false;
                    node.horizon_culled = true;
                }
            }
            for (auto& child : node.children) {
                child.reset();
            }
        }
    }

    void BeginHorizon(const hbg::Vec3& camera_position, const hbg::Vec3* camera_forward) {
        horizon_active_ = false;
        if (!settings_.horizon_culling || camera_forward == nullptr || root_ == nullptr) {
            return;
        }
        if (camera_position.y > root_->zmax) {
            return;
        }
        const float length = std::sqrt(camera_forward->x * camera_forward->x +
                                       camera_forward->z * camera_forward->z);
        if (length < 1.0e-6f) {
            return;
        }
        const float forward_x = camera_forward->x / length;
        const float forward_z = camera_forward->z / length;
        horizon_dir_x_ = -forward_x;
        horizon_dir_z_ = -forward_z;
        horizon_.fill(-std::numeric_limits<float>::infinity());
        horizon_active_ = true;
    }

    bool ProjectOccluder(const QuadNode& node, const hbg::Vec3& camera_position, float* xmin,
                         float* xmax, float* zmin, float* zmax) const {
        const float corners_x[4] = {node.ox, node.ox, node.xmax(), node.xmax()};
        const float corners_z[4] = {node.oy, node.ymax(), node.oy, node.ymax()};
        float projected_x[4];
        float depth[4];
        for (int index = 0; index < 4; ++index) {
            const float dx = corners_x[index] - camera_position.x;
            const float dz = corners_z[index] - camera_position.z;
            projected_x[index] = horizon_dir_z_ * dx - horizon_dir_x_ * dz;
            depth[index] = -horizon_dir_x_ * dx - horizon_dir_z_ * dz;
            if (depth[index] <= 0.0f) {
                return false;
            }
        }

        const float dzmin = node.zmin - camera_position.y;
        const float dzmax = node.zmax - camera_position.y;
        float raw_xmin = projected_x[0] / depth[0];
        float raw_xmax = raw_xmin;
        float raw_zmin = dzmin / depth[0];
        float raw_zmax = dzmax / depth[0];
        for (int index = 1; index < 4; ++index) {
            const float x = projected_x[index] / depth[index];
            raw_xmin = std::min(raw_xmin, x);
            raw_xmax = std::max(raw_xmax, x);
            raw_zmin = std::min(raw_zmin, dzmin / depth[index]);
            raw_zmax = std::max(raw_zmax, dzmax / depth[index]);
        }

        *xmin = raw_xmin * 0.33f + 0.5f;
        *xmax = raw_xmax * 0.33f + 0.5f;
        *zmin = raw_zmin;
        *zmax = raw_zmax;
        return true;
    }

    bool AddOccluder(const QuadNode& node, const hbg::Vec3& camera_position) {
        if (!horizon_active_) {
            return false;
        }

        float xmin = 0.0f;
        float xmax = 0.0f;
        float zmin = 0.0f;
        float zmax = 0.0f;
        if (!ProjectOccluder(node, camera_position, &xmin, &xmax, &zmin, &zmax)) {
            return false;
        }

        int imin =
            std::clamp(static_cast<int>(std::floor(xmin * kHorizonSize)), 0, kHorizonSize - 1);
        int imax =
            std::clamp(static_cast<int>(std::ceil(xmax * kHorizonSize)), 0, kHorizonSize - 1);

        bool occluded = imax >= imin;
        if (occluded) {
            for (int index = imin; index <= imax; ++index) {
                if (zmax > horizon_[index]) {
                    occluded = false;
                    break;
                }
            }
        }

        if (!occluded) {
            imin =
                std::clamp(static_cast<int>(std::ceil(xmin * kHorizonSize)), 0, kHorizonSize - 1);
            imax =
                std::clamp(static_cast<int>(std::floor(xmax * kHorizonSize)), 0, kHorizonSize - 1);
            for (int index = imin; index <= imax; ++index) {
                horizon_[index] = std::max(horizon_[index], zmin);
            }
        }
        return occluded;
    }

    bool IsOccluded(const QuadNode& node, const hbg::Vec3& camera_position) const {
        if (!horizon_active_) {
            return false;
        }

        float xmin = 0.0f;
        float xmax = 0.0f;
        float zmin = 0.0f;
        float zmax = 0.0f;
        if (!ProjectOccluder(node, camera_position, &xmin, &xmax, &zmin, &zmax)) {
            return false;
        }

        const int imin =
            std::clamp(static_cast<int>(std::floor(xmin * kHorizonSize)), 0, kHorizonSize - 1);
        const int imax =
            std::clamp(static_cast<int>(std::ceil(xmax * kHorizonSize)), 0, kHorizonSize - 1);
        for (int index = imin; index <= imax; ++index) {
            if (zmax > horizon_[index]) {
                return false;
            }
        }
        return imax >= imin;
    }

    void Subdivide(QuadNode& node) {
        const float half = node.size * 0.5f;
        const auto make_child = [&](int index, int tx, int ty, float ox, float oy) {
            auto child = std::make_unique<QuadNode>();
            child->parent = &node;
            child->level = node.level + 1;
            child->tx = tx;
            child->ty = ty;
            child->ox = ox;
            child->oy = oy;
            child->size = half;
            child->zmin = node.zmin;
            child->zmax = node.zmax;
            ApplyHeightRange(*child);
            node.children[index] = std::move(child);
        };
        make_child(0, node.tx * 2, node.ty * 2, node.ox, node.oy);
        make_child(1, node.tx * 2 + 1, node.ty * 2, node.ox + half, node.oy);
        make_child(2, node.tx * 2, node.ty * 2 + 1, node.ox, node.oy + half);
        make_child(3, node.tx * 2 + 1, node.ty * 2 + 1, node.ox + half, node.oy + half);
    }

    static void SetChildOrder(QuadNode& node, const hbg::Vec3& camera_position) {
        const float center_x = node.ox + node.size * 0.5f;
        const float center_z = node.oy + node.size * 0.5f;
        if (camera_position.z < center_z) {
            if (camera_position.x < center_x) {
                node.child_order = {0, 1, 2, 3};
            } else {
                node.child_order = {1, 0, 3, 2};
            }
        } else {
            if (camera_position.x < center_x) {
                node.child_order = {2, 0, 3, 1};
            } else {
                node.child_order = {3, 1, 2, 0};
            }
        }
    }

    void Count(const QuadNode& node) {
        ++stats_.node_count;
        stats_.max_depth = std::max(stats_.max_depth, static_cast<std::uint32_t>(node.level));
        if (node.is_leaf()) {
            ++stats_.leaf_count;
            if (node.visible) {
                ++stats_.visible_leaf_count;
            } else if (node.horizon_culled) {
                ++stats_.horizon_culled_leaf_count;
            }
            return;
        }
        for (const auto& child : node.children) {
            Count(*child);
        }
    }

    void CollectLeavesRecursive(const QuadNode& node, std::vector<const QuadNode*>& leaves) const {
        if (node.is_leaf()) {
            leaves.push_back(&node);
            return;
        }
        for (const std::uint8_t order : node.child_order) {
            CollectLeavesRecursive(*node.children[order], leaves);
        }
    }

    QuadSettings settings_;
    float split_distance_ = kSplitDistanceMin;
    std::unique_ptr<QuadNode> root_;
    std::unordered_map<std::uint64_t, std::pair<float, float>> height_ranges_;
    std::function<bool(int, int, int, float&, float&)> height_provider_;
    QuadStats stats_;
    std::array<float, kHorizonSize> horizon_{};
    float horizon_dir_x_ = 0.0f;
    float horizon_dir_z_ = 0.0f;
    bool horizon_active_ = false;
};

bool ReadFloat(const parcel::assets::JsonValue& object, std::string_view key, float& value,
               std::string& error) {
    const parcel::assets::JsonValue* node = object.Find(key);
    if (node == nullptr) {
        return true;
    }
    if (!node->IsNumber() || !std::isfinite(node->number_value) ||
        std::abs(node->number_value) > static_cast<double>(std::numeric_limits<float>::max())) {
        error = std::string(key) + " must be a finite number";
        return false;
    }
    value = static_cast<float>(node->number_value);
    return true;
}

bool ReadUint(const parcel::assets::JsonValue& object, std::string_view key, std::uint32_t& value,
              std::string& error) {
    const parcel::assets::JsonValue* node = object.Find(key);
    if (node == nullptr) {
        return true;
    }
    if (!node->IsNumber() || node->number_value < 0.0 ||
        node->number_value > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        std::floor(node->number_value) != node->number_value) {
        error = std::string(key) + " must be an unsigned integer";
        return false;
    }
    value = static_cast<std::uint32_t>(node->number_value);
    return true;
}

bool ReadBool(const parcel::assets::JsonValue& object, std::string_view key, bool& value,
              std::string& error) {
    const parcel::assets::JsonValue* node = object.Find(key);
    if (node == nullptr) {
        return true;
    }
    if (!node->IsBool()) {
        error = std::string(key) + " must be a boolean";
        return false;
    }
    value = node->bool_value;
    return true;
}

bool ReadString(const parcel::assets::JsonValue& object, std::string_view key, std::string& value,
                std::string& error) {
    const parcel::assets::JsonValue* node = object.Find(key);
    if (node == nullptr) {
        return true;
    }
    if (!node->IsString()) {
        error = std::string(key) + " must be a string";
        return false;
    }
    value = node->string_value;
    return true;
}

bool ReadVec2(const parcel::assets::JsonValue& object, std::string_view key, float& x, float& y,
              std::string& error) {
    const parcel::assets::JsonValue* node = object.Find(key);
    if (node == nullptr) {
        return true;
    }
    if (!node->IsArray() || node->array_value.size() != 2) {
        error = std::string(key) + " must be an array of two numbers";
        return false;
    }
    for (const parcel::assets::JsonValue& component : node->array_value) {
        if (!component.IsNumber() || !std::isfinite(component.number_value) ||
            std::abs(component.number_value) >
                static_cast<double>(std::numeric_limits<float>::max())) {
            error = std::string(key) + " must be an array of two finite numbers";
            return false;
        }
    }
    x = static_cast<float>(node->array_value[0].number_value);
    y = static_cast<float>(node->array_value[1].number_value);
    return true;
}

bool ParseNodeName(std::string_view name, std::string_view prefix, int& level, int& tx, int& ty) {
    if (!name.starts_with(prefix)) {
        return false;
    }
    std::istringstream stream(std::string(name.substr(prefix.size())));
    char first = 0;
    char second = 0;
    if (!(stream >> level >> first >> tx >> second >> ty) || first != '_' || second != '_') {
        return false;
    }
    stream >> std::ws;
    return stream.eof();
}

hbg::Vec3 NormalizeTerrainNormal(hbg::Vec3 normal) {
    const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (!std::isfinite(length) || length <= 1.0e-6f) {
        return hbg::Vec3{0.0f, 1.0f, 0.0f};
    }
    normal.x /= length;
    normal.y /= length;
    normal.z /= length;
    return normal;
}

hbg::asset::Mesh BuildSharedTerrainMesh(const HeightmapTileCache& heightmap,
                                        const TerrainLodSettings& settings, int level, int tx,
                                        int ty, std::uint32_t grid, std::uint32_t material_index) {
    hbg::asset::Mesh mesh;
    const std::uint32_t side = 1U << static_cast<std::uint32_t>(level);
    const float size = settings.world_size / static_cast<float>(side);
    const float ox = settings.origin_x + static_cast<float>(tx) * size;
    const float oz = settings.origin_z + static_cast<float>(ty) * size;
    const float step = size / static_cast<float>(grid - 1U);
    const float world_min_x = settings.origin_x;
    const float world_min_z = settings.origin_z;
    const float world_max_x = settings.origin_x + settings.world_size;
    const float world_max_z = settings.origin_z + settings.world_size;

    const std::size_t top_count = static_cast<std::size_t>(grid) * grid;
    const bool has_skirt = settings.skirt_depth > 0.0f;
    const std::size_t skirt_count = has_skirt ? static_cast<std::size_t>(grid) * 4U : 0U;
    mesh.vertices.reserve(top_count + skirt_count);
    mesh.indices.reserve((top_count - 1U) * 12U +
                         (has_skirt ? static_cast<std::size_t>(grid - 1U) * 24U : 0U));

    const auto sample_height = [&](float x, float z, float& height) {
        return heightmap.SampleHeight(x, z, height);
    };

    for (std::uint32_t z_index = 0; z_index < grid; ++z_index) {
        const float z = oz + static_cast<float>(z_index) * step;
        for (std::uint32_t x_index = 0; x_index < grid; ++x_index) {
            const float x = ox + static_cast<float>(x_index) * step;
            float height = 0.0f;
            if (!sample_height(x, z, height)) {
                height = settings.ground_height;
            }

            const float x0 = std::max(world_min_x, x - step);
            const float x1 = std::min(world_max_x, x + step);
            const float z0 = std::max(world_min_z, z - step);
            const float z1 = std::min(world_max_z, z + step);
            float height_x0 = height;
            float height_x1 = height;
            float height_z0 = height;
            float height_z1 = height;
            sample_height(x0, z, height_x0);
            sample_height(x1, z, height_x1);
            sample_height(x, z0, height_z0);
            sample_height(x, z1, height_z1);

            const float dx = x1 - x0;
            const float dz = z1 - z0;
            const float dhdx = dx > 1.0e-6f ? (height_x1 - height_x0) / dx : 0.0f;
            const float dhdz = dz > 1.0e-6f ? (height_z1 - height_z0) / dz : 0.0f;
            const hbg::Vec3 normal = NormalizeTerrainNormal({-dhdx, 1.0f, -dhdz});

            hbg::asset::MeshVertex vertex;
            vertex.position = hbg::Vec3{x, height, z};
            vertex.normal = normal;
            vertex.tangent = hbg::Vec4{1.0f, 0.0f, 0.0f, -1.0f};
            vertex.texcoord0 = hbg::Vec2{(x - settings.origin_x) / settings.world_size,
                                         (z - settings.origin_z) / settings.world_size};
            vertex.previousPosition = vertex.position;
            mesh.vertices.push_back(vertex);
        }
    }

    const auto top_index = [grid](std::uint32_t x, std::uint32_t z) { return z * grid + x; };
    for (std::uint32_t z = 0; z + 1U < grid; ++z) {
        for (std::uint32_t x = 0; x + 1U < grid; ++x) {
            const std::uint32_t a = top_index(x, z);
            const std::uint32_t b = top_index(x + 1U, z);
            const std::uint32_t c = top_index(x, z + 1U);
            const std::uint32_t d = top_index(x + 1U, z + 1U);
            mesh.indices.push_back(a);
            mesh.indices.push_back(c);
            mesh.indices.push_back(b);
            mesh.indices.push_back(b);
            mesh.indices.push_back(c);
            mesh.indices.push_back(d);
        }
    }

    if (has_skirt) {
        const auto add_side = [&](const std::vector<std::uint32_t>& top, const hbg::Vec3& outward,
                                  bool reverse) {
            const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
            for (const std::uint32_t index : top) {
                hbg::asset::MeshVertex vertex = mesh.vertices[index];
                vertex.position.y -= settings.skirt_depth;
                vertex.normal = NormalizeTerrainNormal(hbg::Vec3{vertex.normal.x + outward.x,
                                                                 vertex.normal.y + outward.y,
                                                                 vertex.normal.z + outward.z});
                vertex.previousPosition = vertex.position;
                mesh.vertices.push_back(vertex);
            }
            for (std::uint32_t index = 0; index + 1U < top.size(); ++index) {
                const std::uint32_t top0 = top[index];
                const std::uint32_t top1 = top[index + 1U];
                const std::uint32_t bottom0 = base + index;
                const std::uint32_t bottom1 = base + index + 1U;
                if (reverse) {
                    mesh.indices.push_back(top0);
                    mesh.indices.push_back(bottom0);
                    mesh.indices.push_back(top1);
                    mesh.indices.push_back(top1);
                    mesh.indices.push_back(bottom0);
                    mesh.indices.push_back(bottom1);
                } else {
                    mesh.indices.push_back(top0);
                    mesh.indices.push_back(top1);
                    mesh.indices.push_back(bottom0);
                    mesh.indices.push_back(top1);
                    mesh.indices.push_back(bottom1);
                    mesh.indices.push_back(bottom0);
                }
            }
        };

        std::vector<std::uint32_t> south;
        std::vector<std::uint32_t> north;
        std::vector<std::uint32_t> west;
        std::vector<std::uint32_t> east;
        south.reserve(grid);
        north.reserve(grid);
        west.reserve(grid);
        east.reserve(grid);
        for (std::uint32_t index = 0; index < grid; ++index) {
            south.push_back(top_index(index, 0));
            north.push_back(top_index(index, grid - 1U));
            west.push_back(top_index(0, index));
            east.push_back(top_index(grid - 1U, index));
        }
        add_side(south, hbg::Vec3{0.0f, 0.0f, -1.0f}, false);
        add_side(east, hbg::Vec3{1.0f, 0.0f, 0.0f}, false);
        add_side(north, hbg::Vec3{0.0f, 0.0f, 1.0f}, true);
        add_side(west, hbg::Vec3{-1.0f, 0.0f, 0.0f}, true);
    }

    mesh.name = "TerrainSharedGrid" + std::to_string(grid) + "_" + std::to_string(level) + "_" +
                std::to_string(tx) + "_" + std::to_string(ty);
    mesh.hasNormals = true;
    mesh.hasTangents = true;
    mesh.hasUv0 = true;
    for (const hbg::asset::MeshVertex& vertex : mesh.vertices) {
        mesh.bounds.expand(vertex.position);
    }

    hbg::asset::SubMesh submesh;
    submesh.indexOffset = 0;
    submesh.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    submesh.materialIndex = material_index;
    submesh.bounds = mesh.bounds;
    mesh.submeshes.push_back(submesh);
    return mesh;
}

} // namespace

struct TerrainLodController::Impl {
    struct Binding {
        std::uint32_t object_index = 0;
        std::uint32_t mesh_index = 0;
        std::uint32_t render_mesh_index = 0;
        int sorting_order = 0;
    };

    TerrainLodSettings settings;
    QuadTree tree;
    QuadStats tree_stats;
    std::shared_ptr<HeightmapTileCache> heightmap;
    std::string heightmap_path;
    std::string heightmap_error;
    TerrainHeightmapStats heightmap_stats;
    hbg::Vec3 last_ground_camera{};
    bool ground_sampled = false;
    std::uint32_t heightmap_cache_tiles = 64;
    std::unordered_map<std::string, std::uint32_t> object_by_name;
    std::unordered_map<std::string, std::pair<float, float>> node_bounds;
    std::vector<Binding> bindings;
    std::vector<bool> bound;
    std::uint32_t bound_object_count = 0;
    std::uint32_t visible_object_count = 0;
    TerrainLodStats stats;
    bool active = false;
    bool water_only = false;
    std::string last_error;
};

TerrainLodController::TerrainLodController() : impl_(std::make_unique<Impl>()) {}
TerrainLodController::~TerrainLodController() = default;
TerrainLodController::TerrainLodController(TerrainLodController&&) noexcept = default;
TerrainLodController& TerrainLodController::operator=(TerrainLodController&&) noexcept = default;

bool TerrainLodController::Load(std::string_view sidecar_json, hbg::render::RenderScene& scene,
                                std::string* error) {
    return Load(sidecar_json, std::span<const std::byte>{}, scene, error);
}

bool TerrainLodController::Load(std::string_view sidecar_json,
                                std::span<const std::byte> heightmap_data,
                                hbg::render::RenderScene& scene, std::string* error) {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    Restore(scene);

    Impl next;
    auto parsed = parcel::assets::ParseJson(sidecar_json);
    if (!parsed) {
        next.last_error = "terrain sidecar json: " + parsed.GetError().message;
        impl_ = std::make_unique<Impl>(std::move(next));
        if (error != nullptr) {
            *error = impl_->last_error;
        }
        return false;
    }
    const parcel::assets::JsonValue& root = parsed.Value();

    std::string parse_error;
    if (!ReadFloat(root, "world_size", next.settings.world_size, parse_error) ||
        !ReadFloat(root, "zmin", next.settings.zmin, parse_error) ||
        !ReadFloat(root, "zmax", next.settings.zmax, parse_error) ||
        !ReadFloat(root, "ground_height", next.settings.ground_height, parse_error) ||
        !ReadFloat(root, "skirt_depth", next.settings.skirt_depth, parse_error) ||
        !ReadFloat(root, "split_factor", next.settings.split_factor, parse_error) ||
        !ReadFloat(root, "viewport_height", next.settings.viewport_height, parse_error) ||
        !ReadBool(root, "split_invisible_quads", next.settings.split_invisible_quads,
                  parse_error) ||
        !ReadBool(root, "horizon_culling", next.settings.horizon_culling, parse_error) ||
        !ReadBool(root, "water_fft", next.settings.water_fft, parse_error) ||
        !ReadFloat(root, "water_fft_tile_size", next.settings.water_fft_tile_size, parse_error) ||
        !ReadFloat(root, "water_fft_amplitude", next.settings.water_fft_amplitude, parse_error) ||
        !ReadFloat(root, "water_fft_update_hz", next.settings.water_fft_update_hz, parse_error) ||
        !ReadFloat(root, "water_sphere_radius", next.settings.water_sphere_radius, parse_error) ||
        !ReadFloat(root, "vegetation_max_distance", next.settings.vegetation_max_distance,
                   parse_error) ||
        !ReadFloat(root, "vegetation_tile_size", next.settings.vegetation_tile_size, parse_error) ||
        !ReadUint(root, "terrain_coarse_grid", next.settings.terrain_coarse_grid, parse_error) ||
        !ReadUint(root, "terrain_fine_grid", next.settings.terrain_fine_grid, parse_error) ||
        !ReadUint(root, "terrain_fine_from_level", next.settings.terrain_fine_from_level,
                  parse_error) ||
        !ReadString(root, "terrain_coarse_mesh", next.settings.terrain_coarse_mesh, parse_error) ||
        !ReadString(root, "terrain_fine_mesh", next.settings.terrain_fine_mesh, parse_error) ||
        !ReadString(root, "name_prefix", next.settings.name_prefix, parse_error) ||
        !ReadVec2(root, "origin", next.settings.origin_x, next.settings.origin_z, parse_error)) {
        next.last_error = "terrain sidecar: " + parse_error;
        impl_ = std::make_unique<Impl>(std::move(next));
        if (error != nullptr) {
            *error = impl_->last_error;
        }
        return false;
    }

    float fov_degrees = next.settings.fov_y_radians * 180.0f / 3.14159265358979323846f;
    if (!ReadFloat(root, "fov_y_degrees", fov_degrees, parse_error) ||
        !ReadUint(root, "max_level", next.settings.max_level, parse_error) ||
        !ReadString(root, "heightmap", next.heightmap_path, parse_error) ||
        !ReadUint(root, "heightmap_cache_tiles", next.heightmap_cache_tiles, parse_error)) {
        next.last_error = "terrain sidecar: " + parse_error;
        impl_ = std::make_unique<Impl>(std::move(next));
        if (error != nullptr) {
            *error = impl_->last_error;
        }
        return false;
    }
    if (!std::isfinite(fov_degrees) || fov_degrees <= 0.0f || fov_degrees >= 180.0f) {
        next.last_error = "terrain sidecar: fov_y_degrees is out of range";
        impl_ = std::make_unique<Impl>(std::move(next));
        if (error != nullptr) {
            *error = impl_->last_error;
        }
        return false;
    }
    next.settings.fov_y_radians = fov_degrees * 3.14159265358979323846f / 180.0f;
    if (!std::isfinite(next.settings.world_size) || next.settings.world_size <= 0.0f) {
        next.last_error = "terrain sidecar: world_size must be positive";
        impl_ = std::make_unique<Impl>(std::move(next));
        if (error != nullptr) {
            *error = impl_->last_error;
        }
        return false;
    }
    if (next.settings.name_prefix.empty()) {
        next.settings.name_prefix = "TerrainLOD_";
    }
    next.settings.max_level = std::min(next.settings.max_level, 16U);
    next.settings.vegetation_max_distance = std::max(0.0f, next.settings.vegetation_max_distance);
    if (!std::isfinite(next.settings.vegetation_tile_size) ||
        next.settings.vegetation_tile_size <= 1.0f) {
        next.settings.vegetation_tile_size = 500.0f;
    }
    next.settings.terrain_coarse_grid = std::clamp(next.settings.terrain_coarse_grid, 2U, 129U);
    next.settings.terrain_fine_grid = std::clamp(next.settings.terrain_fine_grid, 2U, 129U);
    next.settings.terrain_fine_from_level =
        std::clamp(next.settings.terrain_fine_from_level, 0U, 16U);
    const bool shared_requested =
        !next.settings.terrain_coarse_mesh.empty() || !next.settings.terrain_fine_mesh.empty();

    if (!next.heightmap_path.empty()) {
        if (heightmap_data.empty()) {
            next.heightmap_error = "heightmap tile cache: heightmap data was not provided";
        } else {
            auto cache = std::make_shared<HeightmapTileCache>();
            std::string cache_error;
            if (cache->Load(heightmap_data, &cache_error)) {
                cache->SetWorld(next.settings.origin_x, next.settings.origin_z,
                                next.settings.world_size, next.settings.skirt_depth);
                cache->SetCapacity(next.heightmap_cache_tiles);
                next.heightmap = std::move(cache);
                next.heightmap_stats.loaded = true;
                next.heightmap_stats.width = next.heightmap->width();
                next.heightmap_stats.height = next.heightmap->height();
                next.heightmap_stats.height_scale = next.heightmap->height_scale();
                next.tree.set_height_range_provider(
                    [cache = next.heightmap](int level, int tx, int ty, float& zmin, float& zmax) {
                        return cache->Query(level, tx, ty, zmin, zmax);
                    });
            } else {
                next.heightmap_error = std::move(cache_error);
            }
        }
    }

    QuadSettings quad_settings;
    quad_settings.split_factor = next.settings.split_factor;
    quad_settings.max_level = static_cast<int>(next.settings.max_level);
    quad_settings.viewport_height = next.settings.viewport_height;
    quad_settings.fov_y_radians = next.settings.fov_y_radians;
    quad_settings.split_invisible_quads = next.settings.split_invisible_quads;
    quad_settings.horizon_culling = next.settings.horizon_culling;

    next.tree.init(next.settings.origin_x, next.settings.origin_z, next.settings.world_size,
                   next.settings.zmin, next.settings.zmax, quad_settings);

    const parcel::assets::JsonValue* bounds = root.Find("node_bounds");
    if (bounds != nullptr) {
        if (!bounds->IsObject()) {
            next.last_error = "terrain sidecar: node_bounds must be an object";
            impl_ = std::make_unique<Impl>(std::move(next));
            if (error != nullptr) {
                *error = impl_->last_error;
            }
            return false;
        }
        for (const auto& entry : bounds->object_value) {
            const parcel::assets::JsonValue& range = entry.second;
            if (!range.IsArray() || range.array_value.size() != 2 ||
                !range.array_value[0].IsNumber() || !range.array_value[1].IsNumber()) {
                next.last_error = "terrain sidecar: node_bounds." + entry.first +
                                  " must be an array of two numbers";
                impl_ = std::make_unique<Impl>(std::move(next));
                if (error != nullptr) {
                    *error = impl_->last_error;
                }
                return false;
            }
            const float zmin = static_cast<float>(range.array_value[0].number_value);
            const float zmax = static_cast<float>(range.array_value[1].number_value);
            if (!std::isfinite(zmin) || !std::isfinite(zmax)) {
                next.last_error = "terrain sidecar: node_bounds." + entry.first + " must be finite";
                impl_ = std::make_unique<Impl>(std::move(next));
                if (error != nullptr) {
                    *error = impl_->last_error;
                }
                return false;
            }
            next.node_bounds[entry.first] = std::pair<float, float>{zmin, zmax};
            int level = 0;
            int tx = 0;
            int ty = 0;
            if (ParseNodeName(entry.first, std::string_view{}, level, tx, ty)) {
                next.tree.set_node_height_range(level, tx, ty, zmin, zmax);
            }
        }
    }

    bool has_water_object = false;
    next.bound.assign(scene.objects.size(), false);
    next.bindings.resize(scene.objects.size());
    for (std::uint32_t index = 0; index < scene.objects.size(); ++index) {
        const hbg::render::RenderObject& object = scene.objects[index];
        if (object.name == "Water") {
            has_water_object = true;
        }
        int level = 0;
        int tx = 0;
        int ty = 0;
        if (!ParseNodeName(object.name, next.settings.name_prefix, level, tx, ty)) {
            continue;
        }
        if (!next.object_by_name.emplace(object.name, index).second) {
            continue;
        }
        next.bound[index] = true;
        next.bindings[index].object_index = index;
        next.bindings[index].mesh_index = object.meshIndex;
        next.bindings[index].render_mesh_index = object.meshIndex;
        next.bindings[index].sorting_order = object.sortingOrder;
        ++next.bound_object_count;
    }

    if (shared_requested && next.heightmap && !next.object_by_name.empty()) {
        std::uint32_t material_index = UINT32_MAX;
        for (const auto& entry : next.object_by_name) {
            const hbg::render::RenderObject& object = scene.objects[entry.second];
            if (object.meshIndex >= scene.meshes.size()) {
                continue;
            }
            const hbg::asset::Mesh& mesh = scene.meshes[object.meshIndex];
            if (!mesh.submeshes.empty() &&
                mesh.submeshes.front().materialIndex < scene.materials.size()) {
                material_index = mesh.submeshes.front().materialIndex;
                break;
            }
        }

        if (material_index != UINT32_MAX) {
            std::uint32_t shared_mesh_count = 0;
            std::uint32_t shared_vertex_count = 0;
            std::uint32_t shared_triangle_count = 0;
            for (const auto& entry : next.object_by_name) {
                int level = 0;
                int tx = 0;
                int ty = 0;
                if (!ParseNodeName(entry.first, next.settings.name_prefix, level, tx, ty)) {
                    continue;
                }
                const std::uint32_t grid =
                    level >= static_cast<int>(next.settings.terrain_fine_from_level)
                        ? next.settings.terrain_fine_grid
                        : next.settings.terrain_coarse_grid;
                const std::uint32_t mesh_index = static_cast<std::uint32_t>(scene.meshes.size());
                scene.meshes.push_back(BuildSharedTerrainMesh(*next.heightmap, next.settings, level,
                                                              tx, ty, grid, material_index));
                const hbg::asset::Mesh& generated = scene.meshes[mesh_index];

                hbg::render::RenderObject& object = scene.objects[entry.second];
                object.meshIndex = mesh_index;
                object.world = hbg::Mat4::identity();
                object.prevWorld = object.world;
                const float size = next.settings.world_size / static_cast<float>(1 << level);
                const float node_x = next.settings.origin_x + static_cast<float>(tx) * size;
                const float node_z = next.settings.origin_z + static_cast<float>(ty) * size;
                float node_zmin = next.settings.zmin;
                float node_zmax = next.settings.zmax;
                const auto found_bounds = next.node_bounds.find(entry.first);
                if (found_bounds != next.node_bounds.end()) {
                    node_zmin = found_bounds->second.first;
                    node_zmax = found_bounds->second.second;
                }
                object.worldBounds.min = hbg::Vec3{node_x, node_zmin, node_z};
                object.worldBounds.max = hbg::Vec3{node_x + size, node_zmax, node_z + size};
                next.bindings[entry.second].render_mesh_index = mesh_index;
                ++shared_mesh_count;
                shared_vertex_count += static_cast<std::uint32_t>(generated.vertices.size());
                shared_triangle_count += static_cast<std::uint32_t>(generated.indices.size() / 3U);
            }

            if (shared_mesh_count != 0) {
                next.settings.shared_geometry = true;
                next.stats.shared_geometry = true;
                next.stats.shared_mesh_count = shared_mesh_count;
                next.stats.shared_vertex_count = shared_vertex_count;
                next.stats.shared_triangle_count = shared_triangle_count;
                scene.bounds = {};
                for (const hbg::render::RenderObject& object : scene.objects) {
                    scene.bounds.expand(object.worldBounds);
                }
            }
        }
    }

    next.water_only = next.object_by_name.empty() && next.settings.water_fft && has_water_object;
    if (next.object_by_name.empty() && !next.water_only) {
        next.last_error = "terrain sidecar found but no matching objects";
        impl_ = std::make_unique<Impl>(std::move(next));
        if (error != nullptr) {
            *error = impl_->last_error;
        }
        return false;
    }

    next.active = !next.water_only;
    next.stats.bound_object_count = next.bound_object_count;
    next.stats.visible_object_count = 0;
    impl_ = std::make_unique<Impl>(std::move(next));
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void TerrainLodController::Update(hbg::render::RenderScene& scene, const hbg::Vec3& camera_position,
                                  const std::array<hbg::Vec4, 6>* frustum_planes,
                                  const hbg::Vec3* camera_forward) {
    if (!impl_ || !impl_->active) {
        return;
    }
    for (std::uint32_t index = 0; index < impl_->bound.size(); ++index) {
        if (!impl_->bound[index] || index >= scene.objects.size()) {
            continue;
        }
        const Impl::Binding& binding = impl_->bindings[index];
        hbg::render::RenderObject& object = scene.objects[index];
        object.sortingOrder = binding.sorting_order;
        object.meshIndex = UINT32_MAX;
    }

    if (impl_->heightmap) {
        const float dx = camera_position.x - impl_->last_ground_camera.x;
        const float dy = camera_position.y - impl_->last_ground_camera.y;
        const float dz = camera_position.z - impl_->last_ground_camera.z;
        if (!impl_->ground_sampled || dx * dx + dy * dy + dz * dz > 0.01f) {
            float sampled = 0.0f;
            if (impl_->heightmap->SampleHeight(camera_position.x, camera_position.z, sampled)) {
                impl_->settings.ground_height = sampled;
                impl_->ground_sampled = true;
                impl_->last_ground_camera = camera_position;
            }
        }
    }

    impl_->tree.update(camera_position, impl_->settings.ground_height, frustum_planes,
                       camera_forward);
    std::vector<const QuadNode*> leaves;
    impl_->tree.collect_leaves(leaves);
    impl_->visible_object_count = 0;
    std::uint32_t order = 0;
    for (const QuadNode* leaf : leaves) {
        if (leaf == nullptr || !leaf->visible) {
            continue;
        }
        const std::string name = impl_->settings.name_prefix + std::to_string(leaf->level) + "_" +
                                 std::to_string(leaf->tx) + "_" + std::to_string(leaf->ty);
        const auto found = impl_->object_by_name.find(name);
        if (found == impl_->object_by_name.end() || found->second >= scene.objects.size()) {
            continue;
        }
        const Impl::Binding& binding = impl_->bindings[found->second];
        hbg::render::RenderObject& object = scene.objects[found->second];
        object.meshIndex = binding.render_mesh_index;
        object.sortingOrder = static_cast<int>(order++);
        ++impl_->visible_object_count;
    }
    impl_->tree_stats = impl_->tree.stats();
    impl_->stats.node_count = impl_->tree_stats.node_count;
    impl_->stats.leaf_count = impl_->tree_stats.leaf_count;
    impl_->stats.visible_leaf_count = impl_->tree_stats.visible_leaf_count;
    impl_->stats.horizon_culled_leaf_count = impl_->tree_stats.horizon_culled_leaf_count;
    impl_->stats.max_depth = impl_->tree_stats.max_depth;
    impl_->stats.bound_object_count = impl_->bound_object_count;
    impl_->stats.visible_object_count = impl_->visible_object_count;
    impl_->stats.horizon_active = impl_->tree_stats.horizon_active;
    if (impl_->heightmap) {
        const HeightmapTileCache::Stats& cache_stats = impl_->heightmap->stats();
        impl_->heightmap_stats.hits = cache_stats.hits;
        impl_->heightmap_stats.misses = cache_stats.misses;
        impl_->heightmap_stats.loaded_tiles = cache_stats.loaded_tiles;
        impl_->heightmap_stats.evictions = cache_stats.evictions;
        impl_->heightmap_stats.ground_height = impl_->settings.ground_height;
        impl_->heightmap_stats.ground_sampled = impl_->ground_sampled;
    }
}

void TerrainLodController::Restore(hbg::render::RenderScene& scene) {
    if (!impl_) {
        return;
    }
    for (std::uint32_t index = 0; index < impl_->bound.size(); ++index) {
        if (!impl_->bound[index] || index >= scene.objects.size()) {
            continue;
        }
        hbg::render::RenderObject& object = scene.objects[index];
        object.meshIndex = impl_->bindings[index].mesh_index;
        object.sortingOrder = impl_->bindings[index].sorting_order;
    }
    impl_->visible_object_count = 0;
    impl_->stats.visible_object_count = 0;
}

bool TerrainLodController::active() const noexcept { return impl_ && impl_->active; }
bool TerrainLodController::water_only() const noexcept { return impl_ && impl_->water_only; }
const TerrainLodSettings& TerrainLodController::settings() const noexcept {
    static const TerrainLodSettings empty;
    return impl_ ? impl_->settings : empty;
}
const TerrainLodStats& TerrainLodController::stats() const noexcept {
    static const TerrainLodStats empty;
    return impl_ ? impl_->stats : empty;
}
const TerrainHeightmapStats& TerrainLodController::heightmap_stats() const noexcept {
    static const TerrainHeightmapStats empty;
    return impl_ ? impl_->heightmap_stats : empty;
}
const std::string& TerrainLodController::heightmap_error() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->heightmap_error : empty;
}
const std::string& TerrainLodController::last_error() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->last_error : empty;
}

} // namespace parcel_hbg
