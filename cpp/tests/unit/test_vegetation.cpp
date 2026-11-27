#include <catch_amalgamated.hpp>

#include "parcel/hbg/vegetation.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

const std::array<const parcel::assets::TextureAsset*, 2> kNoTextures{{nullptr, nullptr}};

std::string VegetationJson() {
    return R"({
        "count": 4,
        "trees": [
            [10.0, 2.0, 20.0, 2.0, 0.0, 0],
            [11.0, 3.0, 21.0, 1.0, 1.57079632679, 0],
            [12.0, 4.0, 22.0, 3.0, 0.0, 1],
            [13.0, 5.0, 23.0, -1.0, 0.0, 1],
            [14.0, 6.0, 24.0, 1.0, 0.0, 2],
            [15.0, 7.0, 25.0, 1.0, 0.0]
        ]
    })";
}

std::string CullingJson() {
    return R"({
        "trees": [
            [0.0, 0.0, 0.0, 1.0, 0.0, 0],
            [100.0, 0.0, 0.0, 1.0, 0.0, 0],
            [1000.0, 0.0, 0.0, 1.0, 0.0, 1],
            [-1000.0, 0.0, 0.0, 1.0, 0.0, 1]
        ]
    })";
}

} // namespace

TEST_CASE("vegetation builds merged meshes per species tile") {
    hbg::render::RenderScene scene;
    std::vector<parcel_hbg::MaterializedTexture> images;

    auto built = parcel_hbg::BuildVegetation(VegetationJson(), kNoTextures, scene, images);
    REQUIRE(built);
    const parcel_hbg::VegetationStats& stats = built.Value();
    CHECK(stats.built);
    CHECK(stats.instance_count == 3);
    CHECK(stats.deciduous_count == 2);
    CHECK(stats.conifer_count == 1);
    CHECK(stats.mesh_count == 2);
    CHECK(stats.object_count == 2);
    CHECK(stats.tile_count == 1);
    CHECK(stats.submesh_count == 2);
    CHECK(stats.triangle_count == 12);
    CHECK(stats.texture_count == 0);
    CHECK(images.empty());

    REQUIRE(scene.meshes.size() == 2);
    REQUIRE(scene.objects.size() == 2);
    REQUIRE(scene.nodes.empty());
    REQUIRE(scene.materials.size() == 2);
    CHECK(scene.meshes[0].vertices.size() == 16);
    CHECK(scene.meshes[0].indices.size() == 36);
    CHECK(scene.meshes[1].vertices.size() == 8);
    CHECK(scene.meshes[1].indices.size() == 18);
    CHECK(scene.meshes[0].submeshes.size() == 2);
    CHECK(scene.meshes[0].submeshes.front().materialIndex == 0);
    CHECK(scene.meshes[1].submeshes.front().materialIndex == 1);
    CHECK(scene.meshes[0].submeshes[0].indexCount == 24);
    CHECK(scene.meshes[0].submeshes[1].indexCount == 12);
    CHECK(scene.meshes[1].submeshes[0].indexCount == 12);
    CHECK(scene.meshes[1].submeshes[1].indexCount == 6);
    CHECK(scene.materials[0].alphaMode == hbg::asset::AlphaMode::Mask);
    CHECK(scene.materials[0].doubleSided);
    CHECK(scene.materials[0].alphaCutoff == Catch::Approx(0.3f));

    const hbg::asset::MeshVertex& bottom_left = scene.meshes[0].vertices[0];
    CHECK(bottom_left.position.x == Catch::Approx(9.36f));
    CHECK(bottom_left.position.y == Catch::Approx(2.0f));
    CHECK(bottom_left.position.z == Catch::Approx(20.0f));
    CHECK(scene.objects[0].name.starts_with(parcel_hbg::kVegetationObjectPrefix));
    CHECK(scene.objects[0].meshIndex == 0);
    CHECK(scene.objects[1].meshIndex == 1);
    CHECK(scene.objects[0].world.values[12] == Catch::Approx(0.0f));
    CHECK(scene.objects[0].worldBounds.valid());
    CHECK(scene.objects[0].worldBounds.center().x == Catch::Approx(10.34f));
}

TEST_CASE("vegetation culls merged tiles by distance and frustum") {
    hbg::render::RenderScene scene;
    std::vector<parcel_hbg::MaterializedTexture> images;
    auto built = parcel_hbg::BuildVegetation(CullingJson(), kNoTextures, scene, images,
                                             parcel_hbg::VegetationSettings{500.0f, 500.0f});
    REQUIRE(built);
    CHECK(built.Value().tile_count == 3);
    CHECK(built.Value().mesh_count == 3);
    CHECK(built.Value().object_count == 3);

    parcel_hbg::VegetationController controller;
    REQUIRE(controller.Bind(scene, 500.0f));
    CHECK(controller.active());
    CHECK(controller.stats().object_count == 3);
    CHECK(controller.stats().tile_count == 3);
    CHECK(controller.stats().submesh_count == 3);

    controller.Update(scene, hbg::Vec3(0.0f, 0.0f, 0.0f), nullptr, 500.0f);
    CHECK(controller.stats().visible_object_count == 1);
    CHECK(controller.stats().distance_culled_object_count == 2);
    CHECK(scene.objects[0].meshIndex == 0);
    CHECK(scene.objects[1].meshIndex == UINT32_MAX);
    CHECK(scene.objects[2].meshIndex == UINT32_MAX);

    controller.Update(scene, hbg::Vec3(0.0f, 0.0f, 0.0f), nullptr, 0.0f);
    CHECK(controller.stats().visible_object_count == 3);
    CHECK(scene.objects[1].meshIndex == 1);
    CHECK(scene.objects[2].meshIndex == 2);

    std::array<hbg::Vec4, 6> planes{};
    planes[0] = hbg::Vec4(-1.0f, 0.0f, 0.0f, 50.0f);
    controller.Update(scene, hbg::Vec3(0.0f, 0.0f, 0.0f), &planes, 0.0f);
    CHECK(controller.stats().visible_object_count == 2);
    CHECK(controller.stats().frustum_culled_object_count == 1);
    CHECK(scene.objects[0].meshIndex == 0);
    CHECK(scene.objects[1].meshIndex == UINT32_MAX);
    CHECK(scene.objects[2].meshIndex == 2);
}

TEST_CASE("vegetation culls submeshes inside a merged mesh") {
    hbg::render::RenderScene scene;
    std::vector<parcel_hbg::MaterializedTexture> images;
    const std::string json = R"({
        "trees": [
            [0.0, 0.0, 0.0, 1.0, 0.0, 0],
            [600.0, 0.0, 0.0, 1.0, 0.0, 0]
        ]
    })";
    auto built = parcel_hbg::BuildVegetation(json, kNoTextures, scene, images,
                                             parcel_hbg::VegetationSettings{1000.0f, 0.0f, 500.0f});
    REQUIRE(built);
    CHECK(built.Value().mesh_count == 1);
    CHECK(built.Value().object_count == 1);
    CHECK(built.Value().submesh_count == 2);
    REQUIRE(scene.meshes.size() == 1);
    REQUIRE(scene.meshes[0].submeshes.size() == 4);
    const std::uint32_t first_indices = scene.meshes[0].submeshes[0].indexCount;
    const std::uint32_t second_indices = scene.meshes[0].submeshes[2].indexCount;
    const std::uint32_t far_indices = scene.meshes[0].submeshes[3].indexCount;
    CHECK(first_indices > far_indices);
    CHECK(second_indices > far_indices);
    CHECK(far_indices > 0);

    parcel_hbg::VegetationController controller;
    REQUIRE(controller.Bind(scene, 500.0f, 300.0f));
    CHECK(controller.stats().submesh_count == 2);
    controller.Update(scene, hbg::Vec3(0.0f, 0.0f, 0.0f), nullptr, 550.0f);
    CHECK(controller.stats().visible_object_count == 1);
    CHECK(controller.stats().visible_submesh_count == 1);
    CHECK(controller.stats().distance_culled_submesh_count == 1);
    CHECK(scene.meshes[0].submeshes[0].indexCount == first_indices);
    CHECK(scene.meshes[0].submeshes[1].indexCount == 0);
    CHECK(scene.meshes[0].submeshes[2].indexCount == 0);
    CHECK(scene.meshes[0].submeshes[3].indexCount == 0);

    controller.Update(scene, hbg::Vec3(0.0f, 0.0f, 0.0f), nullptr, 0.0f);
    CHECK(controller.stats().visible_submesh_count == 2);
    CHECK(scene.meshes[0].submeshes[0].indexCount == first_indices);
    CHECK(scene.meshes[0].submeshes[1].indexCount == 0);
    CHECK(scene.meshes[0].submeshes[2].indexCount == 0);
    CHECK(scene.meshes[0].submeshes[3].indexCount == far_indices);
    CHECK(scene.meshes[0].submeshes[0].materialIndex == 0);
    CHECK(scene.meshes[0].submeshes[1].materialIndex == UINT32_MAX);
    CHECK(scene.meshes[0].submeshes[2].materialIndex == UINT32_MAX);
    CHECK(scene.meshes[0].submeshes[3].materialIndex == 0);
}

TEST_CASE("vegetation rejects malformed roots") {
    hbg::render::RenderScene scene;
    std::vector<parcel_hbg::MaterializedTexture> images;

    auto built = parcel_hbg::BuildVegetation(R"({"trees": 3})", kNoTextures, scene, images);
    REQUIRE_FALSE(built);
    CHECK(built.GetError().code == parcel::core::ErrorCode::Corrupt);
    CHECK(scene.meshes.empty());

    auto empty = parcel_hbg::BuildVegetation(R"({"trees": []})", kNoTextures, scene, images);
    REQUIRE(empty);
    CHECK(empty.Value().built);
    CHECK(empty.Value().instance_count == 0);
    CHECK(scene.meshes.empty());
    CHECK(scene.objects.empty());
}
