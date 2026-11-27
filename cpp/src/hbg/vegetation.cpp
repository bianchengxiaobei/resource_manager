#include "parcel/hbg/vegetation.hpp"

#include "parcel/assets/json.hpp"

#include "asset/mesh.h"
#include "core/bounds.h"
#include "core/math.h"
#include "render/culling/frustum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace parcel_hbg {
namespace {

using parcel::core::ErrorCode;
using parcel::core::MakeError;

constexpr std::uint32_t kMaxInstances = 1'000'000;
constexpr float kTreeWidth = 0.64f;
constexpr float kDefaultAlphaCutoff = 0.3f;
constexpr float kDefaultTileSize = 500.0f;

struct TreeInstance {
    hbg::Vec3 position{};
    float scale = 1.0f;
    float rotation = 0.0f;
    int species = 0;
};

struct TemplateVertex {
    hbg::Vec3 position;
    hbg::Vec3 normal;
    hbg::Vec4 tangent;
    hbg::Vec2 texcoord;
};

constexpr std::array<TemplateVertex, 8> kTreeVertices{{
    {{-kTreeWidth * 0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
    {{kTreeWidth * 0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
    {{kTreeWidth * 0.5f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
    {{-kTreeWidth * 0.5f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
    {{0.0f, 0.0f, -kTreeWidth * 0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
    {{0.0f, 0.0f, kTreeWidth * 0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
    {{0.0f, 1.0f, kTreeWidth * 0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
    {{0.0f, 1.0f, -kTreeWidth * 0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
}};

constexpr std::array<std::uint32_t, 12> kTreeIndices{{0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7}};
constexpr std::array<std::uint32_t, 6> kTreeFarIndices{{0, 1, 2, 0, 2, 3}};

float ReadNumber(const parcel::assets::JsonValue& value, float fallback) {
    return value.IsNumber() ? static_cast<float>(value.number_value) : fallback;
}

Result<std::array<std::vector<TreeInstance>, 2>> ParseTrees(std::string_view text) {
    auto parsed = parcel::assets::ParseJson(text);
    if (!parsed) {
        return std::move(parsed).TakeError();
    }
    const parcel::assets::JsonValue& root = parsed.Value();
    if (!root.IsObject()) {
        return MakeError(ErrorCode::Corrupt, "vegetation root must be an object");
    }
    const parcel::assets::JsonValue* trees = root.Find("trees");
    if (trees == nullptr || !trees->IsArray()) {
        return MakeError(ErrorCode::Corrupt, "vegetation trees must be an array");
    }
    if (trees->array_value.size() > kMaxInstances) {
        return MakeError(ErrorCode::InvalidArgument, "vegetation instance count exceeds limit");
    }

    std::array<std::vector<TreeInstance>, 2> instances;
    instances[0].reserve(trees->array_value.size() / 2);
    instances[1].reserve(trees->array_value.size() / 2);
    for (const parcel::assets::JsonValue& entry : trees->array_value) {
        if (!entry.IsArray() || entry.array_value.size() < 6) {
            continue;
        }
        const float x = ReadNumber(entry.array_value[0], std::numeric_limits<float>::quiet_NaN());
        const float y = ReadNumber(entry.array_value[1], std::numeric_limits<float>::quiet_NaN());
        const float z = ReadNumber(entry.array_value[2], std::numeric_limits<float>::quiet_NaN());
        const float scale =
            ReadNumber(entry.array_value[3], std::numeric_limits<float>::quiet_NaN());
        const float rotation =
            ReadNumber(entry.array_value[4], std::numeric_limits<float>::quiet_NaN());
        const int species = entry.array_value[5].IsNumber()
                                ? static_cast<int>(entry.array_value[5].number_value)
                                : -1;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(scale) ||
            !std::isfinite(rotation) || scale <= 0.0f || (species != 0 && species != 1)) {
            continue;
        }
        instances[static_cast<std::size_t>(species)].push_back(
            TreeInstance{{x, y, z}, scale, rotation, species});
    }
    return instances;
}

std::int32_t TileCoordinate(float value, float tile_size);
std::uint64_t MakeTileKey(std::int32_t x, std::int32_t z);

hbg::asset::Mesh BuildMergedMesh(std::string_view name, const std::vector<TreeInstance>& trees,
                                 std::uint32_t material_index, float cull_tile_size) {
    hbg::asset::Mesh mesh;
    mesh.name = std::string(name);
    mesh.vertices.reserve(trees.size() * kTreeVertices.size());
    mesh.indices.reserve(trees.size() * kTreeIndices.size());

    const float group_size =
        std::isfinite(cull_tile_size) && cull_tile_size > 1.0f ? cull_tile_size : kDefaultTileSize;
    std::map<std::uint64_t, std::vector<const TreeInstance*>> groups;
    for (const TreeInstance& tree : trees) {
        const std::uint64_t key = MakeTileKey(TileCoordinate(tree.position.x, group_size),
                                              TileCoordinate(tree.position.z, group_size));
        groups[key].push_back(&tree);
    }

    for (const auto& entry : groups) {
        const std::vector<const TreeInstance*>& grouped_trees = entry.second;
        hbg::asset::SubMesh near_sub_mesh;
        near_sub_mesh.indexOffset = static_cast<std::uint32_t>(mesh.indices.size());
        near_sub_mesh.materialIndex = material_index;
        std::vector<std::uint32_t> tree_base_indices;
        tree_base_indices.reserve(grouped_trees.size());
        for (const TreeInstance* tree : grouped_trees) {
            const float c = std::cos(tree->rotation);
            const float s = std::sin(tree->rotation);
            const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
            tree_base_indices.push_back(base);
            for (const TemplateVertex& source : kTreeVertices) {
                hbg::asset::MeshVertex vertex;
                const float local_x = source.position.x * tree->scale;
                const float local_z = source.position.z * tree->scale;
                vertex.position = {c * local_x - s * local_z + tree->position.x,
                                   tree->position.y + source.position.y * tree->scale,
                                   s * local_x + c * local_z + tree->position.z};
                vertex.normal = {c * source.normal.x - s * source.normal.z, source.normal.y,
                                 s * source.normal.x + c * source.normal.z};
                vertex.tangent = {c * source.tangent.x - s * source.tangent.z, source.tangent.y,
                                  s * source.tangent.x + c * source.tangent.z, source.tangent.w};
                vertex.texcoord0 = source.texcoord;
                mesh.vertices.push_back(vertex);
                near_sub_mesh.bounds.expand(vertex.position);
                mesh.bounds.expand(vertex.position);
            }
            for (std::uint32_t index : kTreeIndices) {
                mesh.indices.push_back(base + index);
            }
        }
        near_sub_mesh.indexCount =
            static_cast<std::uint32_t>(mesh.indices.size()) - near_sub_mesh.indexOffset;
        mesh.submeshes.push_back(near_sub_mesh);

        hbg::asset::SubMesh far_sub_mesh;
        far_sub_mesh.indexOffset = static_cast<std::uint32_t>(mesh.indices.size());
        far_sub_mesh.materialIndex = material_index;
        far_sub_mesh.bounds = near_sub_mesh.bounds;
        for (std::uint32_t base : tree_base_indices) {
            for (std::uint32_t index : kTreeFarIndices) {
                mesh.indices.push_back(base + index);
            }
        }
        far_sub_mesh.indexCount =
            static_cast<std::uint32_t>(mesh.indices.size()) - far_sub_mesh.indexOffset;
        mesh.submeshes.push_back(far_sub_mesh);
    }

    mesh.hasNormals = true;
    mesh.hasTangents = true;
    mesh.hasUv0 = true;
    return mesh;
}
hbg::asset::Material BuildMaterial(std::string_view name) {
    hbg::asset::Material material;
    material.name = std::string(name);
    material.alphaMode = hbg::asset::AlphaMode::Mask;
    material.alphaCutoff = kDefaultAlphaCutoff;
    material.doubleSided = true;
    material.metallic = 0.0f;
    material.roughness = 0.9f;
    material.renderQueue = hbg::asset::kRenderQueueAlphaTest;
    return material;
}

void AppendImage(const MaterializedTexture& source, std::string_view name,
                 std::vector<MaterializedTexture>& material_images,
                 hbg::render::RenderScene& scene) {
    hbg::asset::ImageAsset image;
    image.name = std::string(name);
    image.width = source.desc.width;
    image.height = source.desc.height;
    image.mipCount = source.desc.mipCount;
    image.format = source.desc.format;
    if (source.storage) {
        image.pixels.resize(source.storage->size());
        if (!image.pixels.empty()) {
            std::memcpy(image.pixels.data(), source.storage->data(), image.pixels.size());
        }
    }
    scene.images.push_back(std::move(image));
    material_images.push_back(source);
}

float SanitizeTileSize(float tile_size) {
    return std::isfinite(tile_size) && tile_size > 1.0f ? tile_size : kDefaultTileSize;
}

std::int32_t TileCoordinate(float value, float tile_size) {
    return static_cast<std::int32_t>(std::floor(value / tile_size));
}

std::uint64_t MakeTileKey(std::int32_t x, std::int32_t z) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(z));
}

float DistanceToBounds(const hbg::Vec3& point, const hbg::Bounds& bounds) {
    if (!bounds.valid()) {
        return 0.0f;
    }
    const float dx = std::max(std::max(bounds.min.x - point.x, point.x - bounds.max.x), 0.0f);
    const float dy = std::max(std::max(bounds.min.y - point.y, point.y - bounds.max.y), 0.0f);
    const float dz = std::max(std::max(bounds.min.z - point.z, point.z - bounds.max.z), 0.0f);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
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

} // namespace

struct VegetationController::Impl {
    struct SubMeshItem {
        std::uint32_t near_index = 0;
        std::uint32_t near_index_count = 0;
        std::uint32_t near_material_index = UINT32_MAX;
        std::uint32_t far_index = 0;
        std::uint32_t far_index_count = 0;
        std::uint32_t far_material_index = UINT32_MAX;
        bool has_far = false;
        hbg::Bounds bounds;
    };

    struct Item {
        std::uint32_t object_index = 0;
        std::uint32_t mesh_index = 0;
        hbg::Bounds bounds;
        std::vector<SubMeshItem> submeshes;
    };

    struct Tile {
        std::uint64_t key = 0;
        hbg::Bounds bounds;
        std::vector<std::uint32_t> item_indices;
    };

    std::vector<Item> items;
    std::vector<Tile> tiles;
    VegetationCullingStats stats;
    float lod_distance = kDefaultTileSize * 3.0f;
    bool active = false;
};

VegetationController::VegetationController() : impl_(std::make_unique<Impl>()) {}
VegetationController::~VegetationController() = default;
VegetationController::VegetationController(VegetationController&&) noexcept = default;
VegetationController& VegetationController::operator=(VegetationController&&) noexcept = default;

bool VegetationController::Bind(const hbg::render::RenderScene& scene, float tile_size,
                                float lod_distance) {
    impl_ = std::make_unique<Impl>();
    const float size = SanitizeTileSize(tile_size);
    impl_->lod_distance =
        std::isfinite(lod_distance) && lod_distance > 0.0f ? lod_distance : 1500.0f;
    std::unordered_map<std::uint64_t, std::size_t> tile_lookup;
    tile_lookup.reserve(std::min<std::size_t>(scene.objects.size(), 16384));

    for (std::uint32_t object_index = 0; object_index < scene.objects.size(); ++object_index) {
        const hbg::render::RenderObject& object = scene.objects[object_index];
        if (object.meshIndex >= scene.meshes.size() ||
            !object.name.starts_with(kVegetationObjectPrefix)) {
            continue;
        }
        hbg::Bounds bounds = object.worldBounds;
        if (!bounds.valid()) {
            bounds.expand(hbg::Vec3(object.world.values[12], object.world.values[13],
                                    object.world.values[14]));
        }
        const hbg::Vec3 center = bounds.center();
        const std::uint64_t key =
            MakeTileKey(TileCoordinate(center.x, size), TileCoordinate(center.z, size));
        Impl::Item item;
        item.object_index = object_index;
        item.mesh_index = object.meshIndex;
        item.bounds = bounds;
        const hbg::asset::Mesh& mesh = scene.meshes[object.meshIndex];
        item.submeshes.reserve((mesh.submeshes.size() + 1) / 2);
        for (std::size_t submesh_index = 0; submesh_index < mesh.submeshes.size();
             submesh_index += 2) {
            const hbg::asset::SubMesh& near_sub_mesh = mesh.submeshes[submesh_index];
            Impl::SubMeshItem sub_item;
            sub_item.near_index = static_cast<std::uint32_t>(submesh_index);
            sub_item.near_index_count = near_sub_mesh.indexCount;
            sub_item.near_material_index = near_sub_mesh.materialIndex;
            if (submesh_index + 1 < mesh.submeshes.size()) {
                const hbg::asset::SubMesh& far_sub_mesh = mesh.submeshes[submesh_index + 1];
                sub_item.far_index = static_cast<std::uint32_t>(submesh_index + 1);
                sub_item.far_index_count = far_sub_mesh.indexCount;
                sub_item.far_material_index = far_sub_mesh.materialIndex;
                sub_item.has_far = true;
            }
            sub_item.bounds = near_sub_mesh.bounds.valid()
                                  ? hbg::transformBounds(object.world, near_sub_mesh.bounds)
                                  : bounds;
            item.submeshes.push_back(sub_item);
            ++impl_->stats.submesh_count;
        }
        const std::uint32_t item_index = static_cast<std::uint32_t>(impl_->items.size());
        impl_->items.push_back(std::move(item));

        const auto found = tile_lookup.find(key);
        if (found == tile_lookup.end()) {
            Impl::Tile tile;
            tile.key = key;
            tile.bounds = bounds;
            tile.item_indices.push_back(item_index);
            tile_lookup.emplace(key, impl_->tiles.size());
            impl_->tiles.push_back(std::move(tile));
        } else {
            Impl::Tile& tile = impl_->tiles[found->second];
            tile.bounds.expand(bounds);
            tile.item_indices.push_back(item_index);
        }
    }

    std::sort(impl_->tiles.begin(), impl_->tiles.end(),
              [](const Impl::Tile& lhs, const Impl::Tile& rhs) { return lhs.key < rhs.key; });
    impl_->stats.object_count = static_cast<std::uint32_t>(impl_->items.size());
    impl_->stats.tile_count = static_cast<std::uint32_t>(impl_->tiles.size());
    impl_->active = !impl_->items.empty();
    return impl_->active;
}

void VegetationController::Update(hbg::render::RenderScene& scene, const hbg::Vec3& camera_position,
                                  const std::array<hbg::Vec4, 6>* frustum_planes,
                                  float max_distance) {
    if (!impl_ || !impl_->active) {
        return;
    }

    impl_->stats.visible_object_count = 0;
    impl_->stats.visible_tile_count = 0;
    impl_->stats.visible_submesh_count = 0;
    impl_->stats.frustum_culled_object_count = 0;
    impl_->stats.frustum_culled_submesh_count = 0;
    impl_->stats.distance_culled_object_count = 0;
    impl_->stats.distance_culled_submesh_count = 0;
    const bool distance_enabled = std::isfinite(max_distance) && max_distance > 0.0f;

    for (const Impl::Tile& tile : impl_->tiles) {
        const bool tile_frustum =
            frustum_planes == nullptr || IntersectsFrustum(*frustum_planes, tile.bounds);
        const bool tile_distance =
            distance_enabled && DistanceToBounds(camera_position, tile.bounds) > max_distance;
        if (tile_frustum && !tile_distance) {
            ++impl_->stats.visible_tile_count;
        }

        for (std::uint32_t item_index : tile.item_indices) {
            if (item_index >= impl_->items.size()) {
                continue;
            }
            const Impl::Item& item = impl_->items[item_index];
            if (item.object_index >= scene.objects.size()) {
                continue;
            }
            const bool frustum_visible =
                tile_frustum &&
                (frustum_planes == nullptr || IntersectsFrustum(*frustum_planes, item.bounds));
            const bool distance_visible =
                !tile_distance && (!distance_enabled ||
                                   DistanceToBounds(camera_position, item.bounds) <= max_distance);
            const bool visible = frustum_visible && distance_visible;

            hbg::render::RenderObject& object = scene.objects[item.object_index];
            object.meshIndex = visible ? item.mesh_index : UINT32_MAX;
            if (visible) {
                ++impl_->stats.visible_object_count;
                hbg::asset::Mesh& mesh = scene.meshes[item.mesh_index];
                for (const Impl::SubMeshItem& sub_item : item.submeshes) {
                    const bool sub_frustum = frustum_planes == nullptr ||
                                             IntersectsFrustum(*frustum_planes, sub_item.bounds);
                    const float sub_distance = DistanceToBounds(camera_position, sub_item.bounds);
                    const bool sub_distance_visible =
                        !distance_enabled || sub_distance <= max_distance;
                    const bool sub_visible = sub_frustum && sub_distance_visible;
                    const bool far = sub_item.has_far && sub_distance > impl_->lod_distance;
                    if (sub_item.near_index < mesh.submeshes.size()) {
                        hbg::asset::SubMesh& near_mesh = mesh.submeshes[sub_item.near_index];
                        near_mesh.indexCount = sub_visible && !far ? sub_item.near_index_count : 0;
                        near_mesh.materialIndex =
                            sub_visible && !far ? sub_item.near_material_index : UINT32_MAX;
                    }
                    if (sub_item.has_far && sub_item.far_index < mesh.submeshes.size()) {
                        hbg::asset::SubMesh& far_mesh = mesh.submeshes[sub_item.far_index];
                        far_mesh.indexCount = sub_visible && far ? sub_item.far_index_count : 0;
                        far_mesh.materialIndex =
                            sub_visible && far ? sub_item.far_material_index : UINT32_MAX;
                    }
                    if (sub_visible) {
                        ++impl_->stats.visible_submesh_count;
                    } else if (!sub_frustum) {
                        ++impl_->stats.frustum_culled_submesh_count;
                    } else {
                        ++impl_->stats.distance_culled_submesh_count;
                    }
                }
            } else {
                if (item.mesh_index < scene.meshes.size()) {
                    for (hbg::asset::SubMesh& sub_mesh : scene.meshes[item.mesh_index].submeshes) {
                        sub_mesh.indexCount = 0;
                        sub_mesh.materialIndex = UINT32_MAX;
                    }
                }
                if (!frustum_visible) {
                    ++impl_->stats.frustum_culled_object_count;
                } else {
                    ++impl_->stats.distance_culled_object_count;
                }
            }
        }
    }
}

bool VegetationController::active() const noexcept { return impl_ && impl_->active; }

const VegetationCullingStats& VegetationController::stats() const noexcept { return impl_->stats; }

Result<VegetationStats>
BuildVegetation(std::string_view trees_json,
                const std::array<const parcel::assets::TextureAsset*, 2>& textures,
                hbg::render::RenderScene& scene, std::vector<MaterializedTexture>& material_images,
                const VegetationSettings& settings) {
    auto parsed = ParseTrees(trees_json);
    if (!parsed) {
        return std::move(parsed).TakeError();
    }
    if (material_images.size() != scene.images.size()) {
        return MakeError(ErrorCode::InvalidArgument,
                         "vegetation image storage does not match the render scene");
    }

    const std::array<std::vector<TreeInstance>, 2>& instances = parsed.Value();
    const std::array<std::string_view, 2> names{{"Vegetation_Deciduous", "Vegetation_Conifer"}};
    std::array<std::optional<MaterializedTexture>, 2> materialized;
    for (std::size_t species = 0; species < 2; ++species) {
        if (instances[species].empty() || textures[species] == nullptr) {
            continue;
        }
        auto image = MaterializeTexture(*textures[species], names[species]);
        if (!image) {
            return std::move(image).TakeError();
        }
        materialized[species] = std::move(image).Value();
    }

    const float tile_size = SanitizeTileSize(settings.tile_size);
    const float cull_tile_size = SanitizeTileSize(settings.cull_tile_size);
    const float mesh_tile_size = std::max(tile_size, kVegetationMeshTileSize);
    std::unordered_set<std::uint64_t> tile_keys;
    VegetationStats stats;
    stats.built = true;
    stats.deciduous_count = static_cast<std::uint32_t>(instances[0].size());
    stats.conifer_count = static_cast<std::uint32_t>(instances[1].size());
    stats.instance_count = stats.deciduous_count + stats.conifer_count;
    stats.texture_count = 0;

    for (std::size_t species = 0; species < 2; ++species) {
        if (instances[species].empty()) {
            continue;
        }
        const int material_index = static_cast<int>(scene.materials.size());
        hbg::asset::Material material = BuildMaterial(names[species]);
        if (materialized[species]) {
            const int image_index = static_cast<int>(scene.images.size());
            AppendImage(*materialized[species], names[species], material_images, scene);
            material.baseColorTexture = hbg::asset::TextureRef{image_index, 0, {}};
            ++stats.texture_count;
        }
        scene.materials.push_back(std::move(material));
        stats.triangle_count +=
            static_cast<std::uint32_t>(instances[species].size() * kTreeIndices.size() / 3);

        std::map<std::uint64_t, std::vector<TreeInstance>> tile_trees;
        for (const TreeInstance& tree : instances[species]) {
            const std::uint64_t tile_key =
                MakeTileKey(TileCoordinate(tree.position.x, mesh_tile_size),
                            TileCoordinate(tree.position.z, mesh_tile_size));
            tile_trees[tile_key].push_back(tree);
            tile_keys.insert(MakeTileKey(TileCoordinate(tree.position.x, tile_size),
                                         TileCoordinate(tree.position.z, tile_size)));
        }

        for (const auto& [tile_key, trees] : tile_trees) {
            const std::uint32_t mesh_index = static_cast<std::uint32_t>(scene.meshes.size());
            hbg::asset::Mesh mesh =
                BuildMergedMesh(std::string(names[species]) + "_" + std::to_string(tile_key), trees,
                                static_cast<std::uint32_t>(material_index), cull_tile_size);
            const hbg::Bounds world_bounds = mesh.bounds;
            stats.submesh_count += static_cast<std::uint32_t>((mesh.submeshes.size() + 1) / 2);
            scene.meshes.push_back(std::move(mesh));
            ++stats.mesh_count;

            hbg::render::RenderObject object;
            object.name = std::string(kVegetationObjectPrefix) +
                          std::to_string(static_cast<std::uint32_t>(scene.objects.size()));
            object.meshIndex = mesh_index;
            object.worldBounds = world_bounds;
            object.staticShadowCaster = true;
            scene.objects.push_back(std::move(object));
            scene.bounds.expand(world_bounds);
            ++stats.object_count;
        }
    }
    stats.tile_count = static_cast<std::uint32_t>(tile_keys.size());
    return stats;
}

} // namespace parcel_hbg
