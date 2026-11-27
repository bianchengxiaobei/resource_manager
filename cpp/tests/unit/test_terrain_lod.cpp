#include <catch_amalgamated.hpp>

#include "parcel/hbg/terrain_lod.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

std::string TerrainSidecar(bool water_fft = false) {
    return R"({
        "world_size": 100.0,
        "origin": [0.0, 0.0],
        "max_level": 1,
        "split_factor": 2.0,
        "viewport_height": 1080.0,
        "fov_y_degrees": 55.0,
        "ground_height": 0.0,
        "zmin": 0.0,
        "zmax": 10.0,
        "horizon_culling": false,
        "water_fft": )" +
           std::string(water_fft ? "true" : "false") + R"(,
        "name_prefix": "TerrainLOD_",
        "node_bounds": {
            "0_0_0": [0.0, 10.0],
            "1_0_0": [0.0, 10.0],
            "1_1_0": [0.0, 10.0],
            "1_0_1": [0.0, 10.0],
            "1_1_1": [0.0, 10.0]
        }
    })";
}

hbg::render::RenderScene MakeTerrainScene() {
    hbg::render::RenderScene scene;
    scene.meshes.resize(1);
    scene.meshes[0].name = "TerrainSharedGrid";
    scene.meshes[0].submeshes.push_back(hbg::asset::SubMesh{});
    scene.materials.resize(1);
    const char* names[] = {"TerrainLOD_0_0_0", "TerrainLOD_1_0_0", "TerrainLOD_1_1_0",
                           "TerrainLOD_1_0_1", "TerrainLOD_1_1_1"};
    for (std::uint32_t index = 0; index < 5; ++index) {
        hbg::render::RenderObject object;
        object.name = names[index];
        object.meshIndex = 0;
        object.sortingOrder = static_cast<int>(index + 10);
        scene.objects.push_back(std::move(object));
    }
    return scene;
}

std::vector<std::byte> RampHeightmap() {
    constexpr std::uint32_t width = 5;
    constexpr std::uint32_t height = 5;
    std::vector<std::byte> bytes(20 + width * height * sizeof(std::uint16_t));
    std::memcpy(bytes.data(), "HBGHMAP1", 8);
    const auto write_u32 = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t index = 0; index < 4; ++index) {
            bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xffu);
        }
    };
    write_u32(8, width);
    write_u32(12, height);
    const float scale = 1.0f;
    std::uint32_t scale_bits = 0;
    std::memcpy(&scale_bits, &scale, sizeof(scale_bits));
    write_u32(16, scale_bits);
    for (std::uint32_t z = 0; z < height; ++z) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint16_t value = static_cast<std::uint16_t>(x * 10);
            const std::size_t offset = 20 + (z * width + x) * sizeof(std::uint16_t);
            bytes[offset] = static_cast<std::byte>(value & 0xffu);
            bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
        }
    }
    return bytes;
}

hbg::render::RenderScene MakeWaterScene() {
    hbg::render::RenderScene scene;
    scene.meshes.resize(1);
    scene.materials.resize(1);
    hbg::render::RenderObject water;
    water.name = "Water";
    water.meshIndex = 0;
    water.sortingOrder = 7;
    scene.objects.push_back(std::move(water));
    return scene;
}

} // namespace

TEST_CASE("terrain lod selects leaves and hides non-leaf objects") {
    auto scene = MakeTerrainScene();

    parcel_hbg::TerrainLodController controller;
    std::string error;
    REQUIRE(controller.Load(TerrainSidecar(), scene, &error));
    CHECK(error.empty());
    REQUIRE(controller.active());
    CHECK_FALSE(controller.water_only());
    CHECK(controller.stats().bound_object_count == 5);

    controller.Update(scene, hbg::Vec3{0.0f, 0.0f, 0.0f});

    CHECK(controller.stats().leaf_count == 4);
    CHECK(controller.stats().visible_leaf_count == 4);
    CHECK(controller.stats().visible_object_count == 4);
    CHECK(scene.objects[0].meshIndex == UINT32_MAX);
    for (std::size_t index = 1; index < scene.objects.size(); ++index) {
        CHECK(scene.objects[index].meshIndex == 0);
        CHECK(scene.objects[index].sortingOrder == static_cast<int>(index - 1));
    }

    controller.Update(scene, hbg::Vec3{1000.0f, 0.0f, 1000.0f});

    CHECK(controller.stats().leaf_count == 1);
    CHECK(controller.stats().visible_leaf_count == 1);
    CHECK(controller.stats().visible_object_count == 1);
    CHECK(scene.objects[0].meshIndex == 0);
    for (std::size_t index = 1; index < scene.objects.size(); ++index) {
        CHECK(scene.objects[index].meshIndex == UINT32_MAX);
    }

    controller.Restore(scene);

    for (std::size_t index = 0; index < scene.objects.size(); ++index) {
        CHECK(scene.objects[index].meshIndex == 0);
        CHECK(scene.objects[index].sortingOrder == static_cast<int>(index + 10));
    }
}

TEST_CASE("terrain lod applies node bounds to frustum culling") {
    auto scene = MakeTerrainScene();

    const std::string sidecar = R"({
        "world_size": 100.0,
        "origin": [0.0, 0.0],
        "max_level": 1,
        "split_factor": 2.0,
        "viewport_height": 1080.0,
        "fov_y_degrees": 55.0,
        "zmin": 0.0,
        "zmax": 10.0,
        "horizon_culling": false,
        "node_bounds": {
            "0_0_0": [0.0, 10.0],
            "1_0_0": [0.0, 1.0],
            "1_1_0": [0.0, 10.0],
            "1_0_1": [0.0, 1.0],
            "1_1_1": [0.0, 10.0]
        }
    })";

    parcel_hbg::TerrainLodController controller;
    std::string error;
    REQUIRE(controller.Load(sidecar, scene, &error));

    std::array<hbg::Vec4, 6> planes{};
    planes.fill(hbg::Vec4{0.0f, 0.0f, 0.0f, 1.0f});
    planes[5] = hbg::Vec4{0.0f, 1.0f, 0.0f, -5.0f};
    controller.Update(scene, hbg::Vec3{0.0f, 0.0f, 0.0f}, &planes);

    CHECK(controller.stats().leaf_count == 4);
    CHECK(controller.stats().visible_leaf_count == 2);
    CHECK(scene.objects[1].meshIndex == UINT32_MAX);
    CHECK(scene.objects[2].meshIndex == 0);
    CHECK(scene.objects[3].meshIndex == UINT32_MAX);
    CHECK(scene.objects[4].meshIndex == 0);
}

TEST_CASE("terrain lod uses heightmap bounds and camera ground height") {
    auto scene = MakeTerrainScene();
    const std::vector<std::byte> heightmap = RampHeightmap();

    const std::string sidecar = R"({
        "world_size": 100.0,
        "origin": [-50.0, -50.0],
        "max_level": 1,
        "split_factor": 2.0,
        "viewport_height": 1080.0,
        "fov_y_degrees": 55.0,
        "zmin": 0.0,
        "zmax": 50.0,
        "horizon_culling": false,
        "heightmap": "heightmap.bin",
        "heightmap_cache_tiles": 16,
        "node_bounds": {
            "0_0_0": [0.0, 50.0],
            "1_0_0": [0.0, 50.0],
            "1_1_0": [0.0, 50.0],
            "1_0_1": [0.0, 50.0],
            "1_1_1": [0.0, 50.0]
        }
    })";

    parcel_hbg::TerrainLodController controller;
    std::string error;
    REQUIRE(controller.Load(sidecar, heightmap, scene, &error));
    CHECK(error.empty());
    CHECK(controller.heightmap_error().empty());

    std::array<hbg::Vec4, 6> planes{};
    planes.fill(hbg::Vec4{0.0f, 0.0f, 0.0f, 1.0f});
    planes[5] = hbg::Vec4{0.0f, 1.0f, 0.0f, -35.0f};
    controller.Update(scene, hbg::Vec3{0.0f, 0.0f, 0.0f}, &planes);

    CHECK(controller.stats().leaf_count == 4);
    CHECK(controller.stats().visible_leaf_count == 2);
    CHECK(controller.settings().ground_height == Catch::Approx(20.0f));
    CHECK(controller.heightmap_stats().loaded);
    CHECK(controller.heightmap_stats().width == 5);
    CHECK(controller.heightmap_stats().ground_sampled);
    CHECK(controller.heightmap_stats().hits > 0);
    CHECK(controller.heightmap_stats().misses > 0);
    CHECK(scene.objects[1].meshIndex == UINT32_MAX);
    CHECK(scene.objects[2].meshIndex == 0);
    CHECK(scene.objects[3].meshIndex == UINT32_MAX);
    CHECK(scene.objects[4].meshIndex == 0);
}

TEST_CASE("terrain lod water-only sidecar leaves the scene untouched") {
    auto scene = MakeWaterScene();

    parcel_hbg::TerrainLodController controller;
    std::string error;
    REQUIRE(controller.Load(TerrainSidecar(true), scene, &error));
    CHECK(error.empty());
    CHECK_FALSE(controller.active());
    CHECK(controller.water_only());
    CHECK(controller.stats().visible_object_count == 0);

    controller.Update(scene, hbg::Vec3{0.0f, 0.0f, 0.0f});

    REQUIRE(scene.objects.size() == 1);
    CHECK(scene.objects[0].meshIndex == 0);
    CHECK(scene.objects[0].sortingOrder == 7);
}

TEST_CASE("terrain lod rejects a sidecar without matching objects") {
    auto scene = MakeWaterScene();

    parcel_hbg::TerrainLodController controller;
    std::string error;
    CHECK_FALSE(controller.Load(TerrainSidecar(false), scene, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(controller.active());
}

TEST_CASE("terrain lod rejects malformed sidecar fields") {
    auto scene = MakeTerrainScene();

    parcel_hbg::TerrainLodController controller;
    std::string error;
    CHECK_FALSE(controller.Load(R"({"max_level": "four"})", scene, &error));
    CHECK(error.find("max_level") != std::string::npos);
    CHECK_FALSE(controller.active());

    CHECK_FALSE(controller.Load(R"({"world_size": -1})", scene, &error));
    CHECK(error.find("world_size") != std::string::npos);
}

TEST_CASE("terrain lod builds CPU shared grids from heightmap") {
    auto scene = MakeTerrainScene();
    const std::vector<std::byte> heightmap = RampHeightmap();

    const std::string sidecar = R"({
        "world_size": 100.0,
        "origin": [-50.0, -50.0],
        "max_level": 1,
        "split_factor": 2.0,
        "viewport_height": 1080.0,
        "fov_y_degrees": 55.0,
        "zmin": 0.0,
        "zmax": 50.0,
        "skirt_depth": 2.0,
        "horizon_culling": false,
        "heightmap": "heightmap.bin",
        "heightmap_cache_tiles": 16,
        "terrain_coarse_grid": 3,
        "terrain_fine_grid": 5,
        "terrain_fine_from_level": 1,
        "terrain_coarse_mesh": "TerrainSharedGrid3",
        "terrain_fine_mesh": "TerrainSharedGrid5",
        "node_bounds": {
            "0_0_0": [0.0, 50.0],
            "1_0_0": [0.0, 50.0],
            "1_1_0": [0.0, 50.0],
            "1_0_1": [0.0, 50.0],
            "1_1_1": [0.0, 50.0]
        }
    })";

    parcel_hbg::TerrainLodController controller;
    std::string error;
    REQUIRE(controller.Load(sidecar, heightmap, scene, &error));
    CHECK(error.empty());
    CHECK(controller.settings().shared_geometry);
    CHECK(controller.stats().shared_geometry);
    CHECK(controller.stats().shared_mesh_count == 5);
    CHECK(controller.stats().shared_vertex_count == 201);
    CHECK(controller.stats().shared_triangle_count == 280);
    REQUIRE(scene.meshes.size() == 6);

    std::uint32_t root_mesh = UINT32_MAX;
    for (std::uint32_t index = 0; index < 5; ++index) {
        const hbg::render::RenderObject& object = scene.objects[index];
        REQUIRE(object.meshIndex < scene.meshes.size());
        CHECK(object.meshIndex >= 1);
        if (object.name == "TerrainLOD_0_0_0") {
            root_mesh = object.meshIndex;
            CHECK(scene.meshes[root_mesh].name.find("TerrainSharedGrid3_") != std::string::npos);
        } else {
            CHECK(scene.meshes[object.meshIndex].name.find("TerrainSharedGrid5_") !=
                  std::string::npos);
        }
    }
    REQUIRE(root_mesh != UINT32_MAX);
    const hbg::asset::Mesh& root = scene.meshes[root_mesh];
    CHECK(root.vertices.size() == 21);
    CHECK(root.indices.size() == 72);
    CHECK(root.hasNormals);
    CHECK(root.hasUv0);
    CHECK(root.submeshes.size() == 1);
    CHECK(root.submeshes[0].materialIndex == 0);
    CHECK(root.vertices.front().position.y == Catch::Approx(0.0f));
    float max_height = 0.0f;
    for (const hbg::asset::MeshVertex& vertex : root.vertices) {
        max_height = std::max(max_height, vertex.position.y);
    }
    CHECK(max_height == Catch::Approx(40.0f));
    CHECK(root.vertices.front().normal.y > 0.0f);
    CHECK(root.vertices.front().texcoord0.x == Catch::Approx(0.0f));
    CHECK(root.vertices.back().position.y == Catch::Approx(-2.0f));

    controller.Update(scene, hbg::Vec3{0.0f, 0.0f, 0.0f});
    CHECK(scene.objects[1].meshIndex != UINT32_MAX);
    controller.Restore(scene);
    for (const hbg::render::RenderObject& object : scene.objects) {
        CHECK(object.meshIndex == 0);
    }
}