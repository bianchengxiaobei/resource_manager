#include <catch_amalgamated.hpp>

#include "parcel/hbg/water_surface.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace {

hbg::render::RenderScene MakeWaterScene() {
    hbg::render::RenderScene scene;
    scene.meshes.resize(1);
    hbg::asset::Mesh& mesh = scene.meshes[0];
    mesh.name = "Water";
    for (std::uint32_t z = 0; z < 3; ++z) {
        for (std::uint32_t x = 0; x < 3; ++x) {
            hbg::asset::MeshVertex vertex;
            vertex.position =
                hbg::Vec3{static_cast<float>(x) * 10.0f, 0.5f, static_cast<float>(z) * 10.0f};
            vertex.normal = hbg::Vec3{0.0f, 1.0f, 0.0f};
            vertex.texcoord0 =
                hbg::Vec2{static_cast<float>(x) * 0.5f, static_cast<float>(z) * 0.5f};
            mesh.vertices.push_back(vertex);
        }
    }
    mesh.bounds.min = hbg::Vec3{0.0f, 0.5f, 0.0f};
    mesh.bounds.max = hbg::Vec3{20.0f, 0.5f, 20.0f};
    mesh.hasNormals = true;
    mesh.hasUv0 = true;
    mesh.submeshes.push_back({0, 0, 0, mesh.bounds});
    scene.materials.resize(1);
    hbg::render::RenderObject object;
    object.name = "Water";
    object.meshIndex = 0;
    scene.objects.push_back(std::move(object));
    return scene;
}

} // namespace

TEST_CASE("water surface attaches a CPU FFT morph target") {
    auto scene = MakeWaterScene();
    parcel_hbg::WaterSurface water;
    parcel_hbg::WaterSurfaceSettings settings;
    settings.amplitude = 1.0f;
    settings.update_hz = 12.0f;
    std::string error;
    REQUIRE(water.Bind(scene, settings, &error));
    CHECK(error.empty());
    REQUIRE(water.active());
    CHECK(water.stats().mesh_index == 0);
    CHECK(water.stats().vertex_count == 9);
    REQUIRE(scene.meshes[0].morphTargets.size() == 1);
    CHECK(scene.meshes[0].morphTargets[0].hasNormals);
    CHECK(scene.meshes[0].morphTargets[0].positionDeltas.size() == 9);
    REQUIRE(scene.objects[0].morphWeights.size() == 1);
    CHECK(scene.objects[0].morphWeights[0] == 1.0f);

    REQUIRE(water.Update(scene, 0.0f));
    CHECK(water.stats().fft_updates == 1);
    CHECK(water.stats().morph_updates == 1);
    CHECK(water.stats().max_displacement > 0.0f);
    CHECK(water.stats().min_height < water.stats().max_height);
    bool displaced = false;
    for (const hbg::Vec3& delta : scene.meshes[0].morphTargets[0].positionDeltas) {
        displaced = displaced || std::abs(delta.y) > 1.0e-6f;
    }
    CHECK(displaced);

    CHECK_FALSE(water.Update(scene, 0.01f));
    CHECK(water.stats().fft_updates == 1);
    CHECK(water.Update(scene, 0.1f));
    CHECK(water.stats().fft_updates == 2);
    CHECK(water.stats().morph_updates == 2);
}

TEST_CASE("water surface builds the original screen grid") {
    auto scene = MakeWaterScene();
    parcel_hbg::WaterSurface water;
    parcel_hbg::WaterSurfaceSettings settings;
    settings.viewport_width = 80;
    settings.viewport_height = 45;
    std::string error;
    REQUIRE(water.Bind(scene, settings, &error));
    CHECK(error.empty());
    CHECK(water.stats().screen_grid);
    CHECK(water.stats().grid_width >= 2);
    CHECK(water.stats().grid_height >= 2);
    CHECK(scene.meshes[0].vertices.size() ==
          static_cast<std::size_t>(water.stats().grid_width) * water.stats().grid_height);
    const hbg::Mat4 identity = hbg::Mat4::identity();
    const hbg::Vec3 camera{10.0f, 20.0f, 10.0f};
    REQUIRE(water.Update(scene, 0.0f, &identity, &camera));
    CHECK(std::isfinite(water.stats().max_displacement));
    CHECK(water.stats().morph_updates == 1);
}

TEST_CASE("water surface requires the authored Water object") {
    hbg::render::RenderScene scene;
    scene.objects.push_back(hbg::render::RenderObject{});
    parcel_hbg::WaterSurface water;
    std::string error;
    CHECK_FALSE(water.Bind(scene, {}, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(water.active());
}

TEST_CASE("water surface rejects an empty water mesh") {
    hbg::render::RenderScene scene;
    scene.meshes.resize(1);
    hbg::render::RenderObject object;
    object.name = "Water";
    object.meshIndex = 0;
    scene.objects.push_back(std::move(object));
    parcel_hbg::WaterSurface water;
    std::string error;
    CHECK_FALSE(water.Bind(scene, {}, &error));
    CHECK_FALSE(error.empty());
}
