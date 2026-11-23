#include <catch_amalgamated.hpp>

#include "parcel/assets/mesh_asset.hpp"
#include "parcel/core/error.hpp"
#include "parcel/import/mesh_optimizer.hpp"

#include <cstdint>
#include <vector>

using parcel::assets::MeshAsset;

namespace {

MeshAsset MakeDuplicatedQuad() {
    MeshAsset mesh;
    mesh.name = "Quad";
    mesh.vertices.resize(6);
    mesh.vertices[0].position = {0.0F, 0.0F, 0.0F};
    mesh.vertices[1].position = {1.0F, 0.0F, 0.0F};
    mesh.vertices[2].position = {1.0F, 1.0F, 0.0F};
    mesh.vertices[3].position = {0.0F, 0.0F, 0.0F};
    mesh.vertices[4].position = {1.0F, 1.0F, 0.0F};
    mesh.vertices[5].position = {0.0F, 1.0F, 0.0F};
    mesh.indices = {0, 1, 2, 3, 4, 5};
    mesh.bounds.center = {0.5F, 0.5F, 0.0F};
    mesh.bounds.extents = {0.5F, 0.5F, 0.0F};
    parcel::assets::SubMesh sub_mesh;
    sub_mesh.index_offset = 0;
    sub_mesh.index_count = 6;
    sub_mesh.material_index = 0;
    sub_mesh.bounds = mesh.bounds;
    mesh.sub_meshes.push_back(sub_mesh);
    return mesh;
}

} // namespace

TEST_CASE("mesh optimizer remaps, reorders and compacts indexed meshes") {
    MeshAsset mesh = MakeDuplicatedQuad();
    MeshAsset original = mesh;
    auto result = parcel::import::OptimizeMesh(mesh);
    REQUIRE(result);
    CHECK(mesh.vertices.size() == 4);
    CHECK(mesh.indices.size() == original.indices.size());
    CHECK(mesh.sub_meshes.size() == 1);
    CHECK(mesh.sub_meshes[0].index_offset == 0);
    CHECK(mesh.sub_meshes[0].index_count == 6);
    CHECK(mesh.bounds.center.x == original.bounds.center.x);
    CHECK(mesh.bounds.extents.y == original.bounds.extents.y);
    for (std::uint32_t index : mesh.indices) {
        CHECK(index < mesh.vertices.size());
    }

    MeshAsset again = MakeDuplicatedQuad();
    REQUIRE(parcel::import::OptimizeMesh(again));
    CHECK(again.vertices == mesh.vertices);
    CHECK(again.indices == mesh.indices);

    parcel::import::MeshOptimizeOptions options;
    options.optimize_overdraw = true;
    MeshAsset overdraw = MakeDuplicatedQuad();
    REQUIRE(parcel::import::OptimizeMesh(overdraw, options));
    CHECK(overdraw.vertices.size() == 4);
    CHECK(overdraw.indices.size() == 6);
    CHECK(overdraw.sub_meshes[0].index_count == 6);
}

TEST_CASE("mesh optimizer preserves skinned vertices without merging influences") {
    MeshAsset mesh = MakeDuplicatedQuad();
    mesh.skin_vertices.resize(mesh.vertices.size());
    for (std::size_t index = 0; index < mesh.skin_vertices.size(); ++index) {
        mesh.skin_vertices[index].joints = {static_cast<std::uint16_t>(index), 0, 0, 0};
        mesh.skin_vertices[index].weights = {1.0F, 0.0F, 0.0F, 0.0F};
    }
    auto result = parcel::import::OptimizeMesh(mesh);
    REQUIRE(result);
    CHECK(mesh.vertices.size() == 6);
    REQUIRE(mesh.skin_vertices.size() == 6);
    for (std::size_t index = 0; index < mesh.skin_vertices.size(); ++index) {
        CHECK(mesh.skin_vertices[index].joints[0] == index);
    }

    MeshAsset invalid = MakeDuplicatedQuad();
    invalid.skin_vertices.resize(3);
    auto invalid_result = parcel::import::OptimizeMesh(invalid);
    REQUIRE_FALSE(invalid_result);
    CHECK(invalid_result.GetError().code == parcel::core::ErrorCode::InvalidArgument);
}

TEST_CASE("mesh optimizer preserves morph target deltas without merging vertices") {
    MeshAsset mesh = MakeDuplicatedQuad();
    parcel::assets::MorphTarget target;
    target.name = "Smile";
    target.position_deltas.resize(mesh.vertices.size());
    for (std::size_t index = 0; index < target.position_deltas.size(); ++index) {
        target.position_deltas[index] = {static_cast<float>(index) * 0.1F, 0.0F, 0.0F};
    }
    mesh.morph_targets.push_back(target);
    mesh.morph_weights = {0.5F};
    auto result = parcel::import::OptimizeMesh(mesh);
    REQUIRE(result);
    CHECK(mesh.vertices.size() == 6);
    REQUIRE(mesh.morph_targets.size() == 1);
    CHECK(mesh.morph_targets[0].position_deltas.size() == 6);
    CHECK(mesh.morph_targets[0].position_deltas[5].x == Catch::Approx(0.5F));

    MeshAsset invalid = MakeDuplicatedQuad();
    parcel::assets::MorphTarget invalid_target;
    invalid_target.position_deltas.resize(3);
    invalid.morph_targets.push_back(invalid_target);
    auto invalid_result = parcel::import::OptimizeMesh(invalid);
    REQUIRE_FALSE(invalid_result);
    CHECK(invalid_result.GetError().code == parcel::core::ErrorCode::InvalidArgument);
}

TEST_CASE("mesh optimizer preserves submesh ranges and rejects invalid data") {
    MeshAsset mesh = MakeDuplicatedQuad();
    parcel::assets::SubMesh second;
    second.index_offset = 6;
    second.index_count = 6;
    second.material_index = 1;
    second.bounds = mesh.bounds;
    mesh.indices.push_back(0);
    mesh.indices.push_back(1);
    mesh.indices.push_back(2);
    mesh.indices.push_back(3);
    mesh.indices.push_back(4);
    mesh.indices.push_back(5);
    mesh.sub_meshes.push_back(second);
    auto result = parcel::import::OptimizeMesh(mesh);
    REQUIRE(result);
    REQUIRE(mesh.sub_meshes.size() == 2);
    CHECK(mesh.sub_meshes[0].index_offset == 0);
    CHECK(mesh.sub_meshes[0].index_count == 6);
    CHECK(mesh.sub_meshes[1].index_offset == 6);
    CHECK(mesh.sub_meshes[1].index_count == 6);

    MeshAsset invalid_index = MakeDuplicatedQuad();
    invalid_index.indices[0] = 99;
    auto invalid = parcel::import::OptimizeMesh(invalid_index);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.GetError().code == parcel::core::ErrorCode::InvalidArgument);

    MeshAsset invalid_threshold = MakeDuplicatedQuad();
    parcel::import::MeshOptimizeOptions options;
    options.optimize_overdraw = true;
    options.overdraw_threshold = 0.0F;
    auto threshold = parcel::import::OptimizeMesh(invalid_threshold, options);
    REQUIRE_FALSE(threshold);
    CHECK(threshold.GetError().code == parcel::core::ErrorCode::InvalidArgument);
}
