#include <catch_amalgamated.hpp>

#include "parcel/hbg/river.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

hbg::render::RenderScene MakeRiverTerrainScene() {
    hbg::render::RenderScene scene;
    scene.meshes.resize(1);
    hbg::asset::Mesh& mesh = scene.meshes[0];
    mesh.name = "RiverTerrain";
    constexpr std::uint32_t kSize = 5;
    for (std::uint32_t z = 0; z < kSize; ++z) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            hbg::asset::MeshVertex vertex;
            vertex.position = hbg::Vec3{static_cast<float>(x), 0.0f, static_cast<float>(z)};
            vertex.normal = hbg::Vec3{0.0f, 1.0f, 0.0f};
            vertex.tangent = hbg::Vec4{1.0f, 0.0f, 0.0f, 1.0f};
            vertex.texcoord0 =
                hbg::Vec2{static_cast<float>(x) / 4.0f, static_cast<float>(z) / 4.0f};
            mesh.vertices.push_back(vertex);
        }
    }
    for (std::uint32_t z = 0; z + 1 < kSize; ++z) {
        for (std::uint32_t x = 0; x + 1 < kSize; ++x) {
            const std::uint32_t a = z * kSize + x;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + kSize;
            const std::uint32_t d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    mesh.hasNormals = true;
    mesh.hasTangents = true;
    mesh.hasUv0 = true;
    hbg::Bounds bounds;
    for (const hbg::asset::MeshVertex& vertex : mesh.vertices) {
        bounds.expand(vertex.position);
    }
    mesh.bounds = bounds;
    mesh.submeshes.push_back({0, static_cast<std::uint32_t>(mesh.indices.size()), 0, mesh.bounds});
    scene.materials.resize(1);
    scene.materials[0].name = "M_RiverTerrain";
    hbg::render::RenderObject object;
    object.name = "RiverTerrain";
    object.meshIndex = 0;
    object.worldBounds = mesh.bounds;
    scene.objects.push_back(std::move(object));
    return scene;
}

parcel_hbg::RiverSettings MakeRiverSettings() {
    parcel_hbg::RiverSettings settings;
    settings.length = 20.0f;
    settings.width = 10.0f;
    settings.amplitude = 0.0f;
    settings.waves = 0.0f;
    settings.segments = 32;
    settings.cross_segments = 2;
    settings.base_height = 0.05f;
    settings.bed_depth = 1.5f;
    settings.bed_width_factor = 0.9f;
    settings.bank_width_factor = 2.2f;
    return settings;
}

} // namespace

TEST_CASE("river scene builds the original meander ribbon") {
    auto scene = MakeRiverTerrainScene();
    parcel_hbg::RiverScene river;
    std::string error;
    REQUIRE(river.Apply(scene, MakeRiverSettings(), &error));
    CHECK(error.empty());
    REQUIRE(river.active());
    const parcel_hbg::RiverStats& stats = river.stats();
    CHECK(stats.vertex_count == 99);
    CHECK(stats.index_count == 384);
    CHECK(stats.flow_samples > 0);
    CHECK(std::isfinite(stats.min_speed));
    CHECK(std::isfinite(stats.max_speed));
    CHECK(scene.meshes.size() == 2);
    CHECK(scene.objects.size() == 2);
    CHECK(scene.objects.back().name == "RiverSurface");
    CHECK(scene.meshes.back().name == "RiverSurface");
    CHECK(scene.objects.back().meshIndex == stats.mesh_index);
    CHECK(scene.materials[stats.material_index].alphaMode == hbg::asset::AlphaMode::Blend);
    CHECK(scene.materials[stats.material_index].renderQueue == hbg::asset::kRenderQueueTransparent);
}

TEST_CASE("river scene carves the CPU terrain mesh") {
    auto scene = MakeRiverTerrainScene();
    const hbg::Bounds original_bounds = scene.meshes[0].bounds;
    parcel_hbg::RiverScene river;
    REQUIRE(river.Apply(scene, MakeRiverSettings()));
    const parcel_hbg::RiverStats& stats = river.stats();
    CHECK(stats.terrain_deformed);
    CHECK(stats.terrain_mesh_name == "RiverTerrain");
    CHECK(stats.terrain_changed_vertex_count > 0);
    CHECK(stats.terrain_min_delta < 0.0f);
    CHECK(scene.meshes[0].bounds.min.y < original_bounds.min.y);
    CHECK(scene.objects[0].worldBounds.min.y < original_bounds.min.y);
    CHECK(scene.meshes[0].submeshes[0].bounds.min.y == scene.meshes[0].bounds.min.y);
    bool changed = false;
    for (const hbg::asset::MeshVertex& vertex : scene.meshes[0].vertices) {
        changed = changed || vertex.position.y < -1.0e-3f;
    }
    CHECK(changed);
    for (const hbg::asset::MeshVertex& vertex : scene.meshes[0].vertices) {
        CHECK(std::isfinite(vertex.normal.x));
        CHECK(std::isfinite(vertex.normal.y));
        CHECK(std::isfinite(vertex.normal.z));
        CHECK(std::isfinite(vertex.tangent.x));
        CHECK(std::isfinite(vertex.tangent.y));
        CHECK(std::isfinite(vertex.tangent.z));
    }
}

TEST_CASE("river scene generates the original water material textures") {
    auto scene = MakeRiverTerrainScene();
    std::vector<parcel_hbg::MaterializedTexture> images;
    parcel_hbg::RiverSettings settings = MakeRiverSettings();
    settings.texture_size = 32;
    settings.wave_tile_size = 32;
    settings.wave_time_loop = 1;
    settings.particles.enabled = false;
    parcel_hbg::RiverScene river;
    std::string error;
    REQUIRE(river.Apply(scene, settings, &error, &images));
    CHECK(error.empty());
    const parcel_hbg::RiverStats& stats = river.stats();
    CHECK(stats.textures_generated);
    CHECK(stats.texture_size == 32);
    CHECK(stats.base_color_image_index == 0);
    CHECK(stats.normal_image_index == 1);
    REQUIRE(scene.images.size() == 2);
    REQUIRE(images.size() == 2);
    CHECK(scene.images[0].name == "RiverBaseColor");
    CHECK(scene.images[1].name == "RiverNormal");
    CHECK(scene.images[0].width == 32);
    CHECK(scene.images[0].height == 32);
    CHECK(scene.images[0].pixels.size() == 32U * 32U * 4U);
    CHECK(scene.images[1].pixels.size() == 32U * 32U * 4U);
    CHECK(images[0].storage->size() == scene.images[0].pixels.size());
    CHECK(images[1].storage->size() == scene.images[1].pixels.size());

    const hbg::asset::Material& material = scene.materials[stats.material_index];
    CHECK(material.baseColorTexture.imageIndex == 0);
    CHECK(material.normalTexture.imageIndex == 1);
    CHECK(material.baseColorTexture.sampler.wrapS == hbg::asset::TextureWrap::Repeat);
    CHECK(material.normalTexture.sampler.wrapT == hbg::asset::TextureWrap::Repeat);
    CHECK(material.baseColorFactor == hbg::Vec4{1.0f, 1.0f, 1.0f, 0.9f});
    CHECK(material.normalScale == Catch::Approx(0.35f));
    CHECK(scene.images[0].pixels[(16U * 32U + 16U) * 4U + 3U] > 0);
    CHECK(scene.images[1].pixels[(16U * 32U + 16U) * 4U + 3U] == 255);
    CHECK(stats.normal_frame_count == 1);
    CHECK(stats.normal_frame_index == 0);
    CHECK(stats.normal_frame_step == Catch::Approx(5.0f));
}

TEST_CASE("river scene animates the original wave normal frames") {
    auto scene = MakeRiverTerrainScene();
    std::vector<parcel_hbg::MaterializedTexture> images;
    parcel_hbg::RiverSettings settings = MakeRiverSettings();
    settings.texture_size = 16;
    settings.wave_tile_size = 16;
    settings.wave_time_loop = 3;
    settings.wave_time_step = 0.1f;
    settings.particles.enabled = false;
    parcel_hbg::RiverScene river;
    std::string error;
    REQUIRE(river.Apply(scene, settings, &error, &images));
    CHECK(error.empty());
    const parcel_hbg::RiverStats& stats = river.stats();
    CHECK(stats.textures_generated);
    CHECK(stats.normal_frame_count == 3);
    CHECK(stats.normal_frame_index == 0);
    CHECK(stats.normal_frame_time == 0.0f);
    CHECK(stats.normal_frame_step == Catch::Approx(0.1f));
    REQUIRE(scene.images.size() == 4);
    REQUIRE(images.size() == 4);
    CHECK(scene.images[1].name == "RiverNormal");
    CHECK(scene.images[2].name == "RiverNormalFrame1");
    CHECK(scene.images[3].name == "RiverNormalFrame2");
    CHECK(scene.images[1].pixels != scene.images[2].pixels);
    REQUIRE(scene.meshes.size() == 4);
    REQUIRE(scene.materials.size() == 4);
    for (std::uint32_t frame = 0; frame < stats.normal_frame_count; ++frame) {
        const std::uint32_t material_index =
            scene.meshes[stats.mesh_index + frame].submeshes.front().materialIndex;
        CHECK(scene.materials[material_index].normalTexture.imageIndex ==
              static_cast<int>(1 + frame));
    }
    REQUIRE(scene.objects[stats.object_index].meshIndex == stats.mesh_index);

    CHECK_FALSE(river.Update(scene, 0.0f));
    REQUIRE(river.Update(scene, 0.1f));
    CHECK(stats.normal_frame_index == 1);
    CHECK(stats.normal_frame_time == Catch::Approx(0.1f));
    CHECK(scene.objects[stats.object_index].meshIndex == stats.mesh_index + 1);
    CHECK_FALSE(river.Update(scene, 0.05f));
    CHECK(stats.normal_frame_index == 1);
    REQUIRE(river.Update(scene, 0.05f));
    CHECK(stats.normal_frame_index == 2);
    CHECK(scene.objects[stats.object_index].meshIndex == stats.mesh_index + 2);
    REQUIRE(river.Update(scene, 0.1f));
    CHECK(stats.normal_frame_index == 0);
    CHECK(stats.normal_frame_time == Catch::Approx(0.3f));
    CHECK(scene.objects[stats.object_index].meshIndex == stats.mesh_index);
}

TEST_CASE("river scene parses the original sidecar object") {
    auto scene = MakeRiverTerrainScene();
    std::vector<parcel_hbg::MaterializedTexture> images;
    parcel_hbg::RiverScene river;
    std::string error;
    const std::string json = R"({
        "river": {
            "length": 20.0,
            "width": 10.0,
            "amplitude": 0.0,
            "waves": 0.0,
            "segments": 16,
            "crossSegments": 2,
            "baseHeight": 0.05,
            "uvLength": 20.0,
            "material": "M_River",
            "deform": true,
            "terrain": "RiverTerrain",
            "terrainCarveDepth": 1.5,
            "terrainBedWidth": 0.9,
            "terrainBankWidth": 2.2,
            "baseColor": [1.0, 1.0, 1.0, 1.0],
            "waveTimeLoop": 1,
            "particleCapacity": 8,
            "particleRadius": 64.0,
            "foamTextureSize": 32
        }})";
    REQUIRE(river.Apply(scene, json, &error, &images));
    CHECK(error.empty());
    CHECK(river.stats().vertex_count == 51);
    CHECK(river.stats().index_count == 192);
    CHECK(river.stats().terrain_deformed);
    CHECK(river.stats().foam_generated);
    CHECK(river.stats().foam_particle_count == 8);
    CHECK(river.stats().foam_texture_size == 32);
    CHECK(scene.images.size() == 3);
    CHECK(images.size() == 3);
    CHECK(scene.materials[river.stats().material_index].baseColorTexture.imageIndex == 0);
}

TEST_CASE("river lifecycle fade follows the original particle layer") {
    CHECK(parcel_hbg::RiverLifecycleFade(0.0f, 2.0f, 30.0f, 2.0f) == 0.0f);
    CHECK(parcel_hbg::RiverLifecycleFade(1.0f, 2.0f, 30.0f, 2.0f) == Catch::Approx(0.5f));
    CHECK(parcel_hbg::RiverLifecycleFade(31.0f, 2.0f, 30.0f, 2.0f) == Catch::Approx(1.0f));
    CHECK(parcel_hbg::RiverLifecycleFade(33.0f, 2.0f, 30.0f, 2.0f) == Catch::Approx(0.5f));
    CHECK(parcel_hbg::RiverLifecycleFade(40.0f, 2.0f, 30.0f, 2.0f) == 0.0f);
}

TEST_CASE("river scene builds CPU foam particles and a transparent overlay") {
    auto scene = MakeRiverTerrainScene();
    std::vector<parcel_hbg::MaterializedTexture> images;
    parcel_hbg::RiverSettings settings = MakeRiverSettings();
    settings.texture_size = 32;
    settings.wave_tile_size = 32;
    settings.wave_time_loop = 1;
    settings.particles.enabled = true;
    settings.particles.max_particles = 16;
    settings.particles.texture_size = 32;
    settings.particles.screen_radius = 64.0f;
    parcel_hbg::RiverScene river;
    std::string error;
    REQUIRE(river.Apply(scene, settings, &error, &images));
    CHECK(error.empty());
    const parcel_hbg::RiverStats& stats = river.stats();
    REQUIRE(stats.foam_generated);
    CHECK(stats.foam_particle_count == 16);
    CHECK(stats.foam_vertex_count == 64);
    CHECK(stats.foam_index_count == 96);
    CHECK(stats.foam_texture_size == 32);
    CHECK(stats.foam_texture_image_index == 2);
    CHECK(stats.foam_min_fade <= stats.foam_max_fade);
    CHECK(stats.foam_min_fade >= 0.0f);
    CHECK(stats.foam_max_fade <= 1.0f);
    CHECK_FALSE(stats.foam_screen_space);
    CHECK(stats.foam_screen_particle_count == 0);
    REQUIRE(scene.images.size() == 3);
    REQUIRE(images.size() == 3);
    CHECK(scene.images[2].name == "RiverFoam");
    CHECK(scene.images[2].width == 32);
    CHECK(scene.images[2].pixels.size() == 32U * 32U * 4U);
    CHECK(scene.images[2].pixels[(16U * 32U + 16U) * 4U + 3U] > 0);
    CHECK(scene.images[2].pixels[3U] == 0);

    REQUIRE(stats.foam_mesh_index == 2);
    REQUIRE(stats.foam_object_index == 2);
    REQUIRE(scene.meshes.size() == 3);
    REQUIRE(scene.objects.size() == 3);
    CHECK(scene.meshes[stats.foam_mesh_index].name == "RiverFoam");
    CHECK(scene.objects[stats.foam_object_index].name == "RiverFoam");
    CHECK(scene.objects[stats.foam_object_index].meshIndex == stats.foam_mesh_index);
    CHECK(scene.meshes[stats.foam_mesh_index].vertices.size() == 64);
    CHECK(scene.meshes[stats.foam_mesh_index].indices.size() == 96);
    CHECK(scene.meshes[stats.foam_mesh_index].bounds.valid());
    for (const hbg::asset::MeshVertex& vertex : scene.meshes[stats.foam_mesh_index].vertices) {
        CHECK(std::isfinite(vertex.position.x));
        CHECK(std::isfinite(vertex.position.y));
        CHECK(std::isfinite(vertex.position.z));
        CHECK(vertex.normal.y == Catch::Approx(1.0f));
    }

    const hbg::asset::Material& material = scene.materials[stats.foam_material_index];
    CHECK(material.name == "M_RiverFoam");
    CHECK(material.alphaMode == hbg::asset::AlphaMode::Mask);
    CHECK(material.renderQueue == hbg::asset::kRenderQueueAlphaTest);
    CHECK(material.alphaCutoff == Catch::Approx(0.2f));
    CHECK(material.doubleSided);
    CHECK(material.baseColorTexture.imageIndex == 2);
}

TEST_CASE("river foam particles animate through a morph target") {
    auto scene = MakeRiverTerrainScene();
    std::vector<parcel_hbg::MaterializedTexture> images;
    parcel_hbg::RiverSettings settings = MakeRiverSettings();
    settings.texture_size = 32;
    settings.wave_tile_size = 32;
    settings.wave_time_loop = 1;
    settings.particles.enabled = true;
    settings.particles.max_particles = 16;
    settings.particles.texture_size = 32;
    settings.particles.screen_radius = 64.0f;
    parcel_hbg::RiverScene river;
    std::string error;
    REQUIRE(river.Apply(scene, settings, &error, &images));
    CHECK(error.empty());
    const parcel_hbg::RiverStats& stats = river.stats();
    REQUIRE(stats.foam_dynamic);
    REQUIRE(stats.foam_morph_target_index != UINT32_MAX);
    hbg::asset::Mesh& mesh = scene.meshes[stats.foam_mesh_index];
    REQUIRE(mesh.morphTargets.size() > stats.foam_morph_target_index);
    hbg::asset::MorphTarget& target = mesh.morphTargets[stats.foam_morph_target_index];
    CHECK(target.name == "RiverFoamFlow");
    CHECK(target.positionDeltas.size() == stats.foam_vertex_count);
    REQUIRE(scene.objects[stats.foam_object_index].morphWeights.size() == 1);
    CHECK(scene.objects[stats.foam_object_index].morphWeights[0] == 1.0f);
    CHECK(stats.foam_updates == 0);
    CHECK(stats.foam_time == 0.0f);
    CHECK(stats.foam_max_displacement > 0.0f);
    const std::vector<hbg::Vec3> initial_deltas = target.positionDeltas;

    CHECK_FALSE(river.Update(scene, 0.0f));
    REQUIRE(river.Update(scene, 0.1f));
    CHECK(stats.foam_updates == 1);
    CHECK(stats.foam_time == Catch::Approx(0.1f));
    CHECK(stats.foam_min_fade >= 0.0f);
    CHECK(stats.foam_min_fade <= stats.foam_max_fade);
    CHECK(stats.foam_max_fade <= 1.0f);
    CHECK(stats.foam_max_displacement > 0.0f);
    bool changed = false;
    for (std::size_t index = 0; index < target.positionDeltas.size(); ++index) {
        const hbg::Vec3& delta = target.positionDeltas[index];
        const hbg::Vec3& initial = initial_deltas[index];
        changed = changed || delta.x != initial.x || delta.y != initial.y || delta.z != initial.z;
    }
    CHECK(changed);

    REQUIRE(river.Update(scene, 0.05f));
    CHECK(stats.foam_updates == 2);
    CHECK(stats.foam_time == Catch::Approx(0.15f));
    river.Close();
    CHECK_FALSE(river.active());
    CHECK_FALSE(river.Update(scene, 0.05f));
}

TEST_CASE("river scene seeds the original screen-space poisson foam") {
    auto scene = MakeRiverTerrainScene();
    std::vector<parcel_hbg::MaterializedTexture> images;
    parcel_hbg::RiverSettings settings = MakeRiverSettings();
    settings.texture_size = 32;
    settings.wave_tile_size = 32;
    settings.wave_time_loop = 1;
    settings.particles.enabled = true;
    settings.particles.max_particles = 64;
    settings.particles.texture_size = 32;
    settings.particles.screen_radius = 8.0f;
    parcel_hbg::RiverScene river;
    std::string error;
    REQUIRE(river.Apply(scene, settings, &error, &images));
    CHECK(error.empty());
    const parcel_hbg::RiverStats& stats = river.stats();
    REQUIRE(stats.foam_dynamic);
    CHECK_FALSE(stats.foam_screen_space);
    CHECK(stats.foam_screen_particle_count == 0);

    const hbg::Vec3 eye{0.0f, 25.0f, 0.0f};
    const hbg::Mat4 view_matrix =
        hbg::lookAtRH(eye, hbg::Vec3{0.0f, 0.0f, 0.0f}, hbg::Vec3{0.0f, 0.0f, -1.0f});
    const hbg::Mat4 projection = hbg::perspectiveRH(1.0f, 160.0f / 120.0f, 0.1f, 100.0f, false);
    parcel_hbg::RiverView view;
    view.view_proj = projection * view_matrix;
    view.inv_view_proj = hbg::inverse(view.view_proj);
    view.camera_position = eye;
    view.viewport_width = 160;
    view.viewport_height = 120;

    REQUIRE(river.Update(scene, 0.1f, &view));
    CHECK(stats.foam_screen_space);
    CHECK(stats.foam_screen_particle_count > 0);
    CHECK(stats.foam_screen_particle_count <= stats.foam_particle_count);
    CHECK(stats.foam_updates == 1);
    CHECK(stats.foam_min_fade >= 0.0f);
    CHECK(stats.foam_min_fade <= stats.foam_max_fade);
    CHECK(stats.foam_max_fade <= 1.0f);
    CHECK(stats.foam_max_displacement > 0.0f);
    REQUIRE(scene.meshes[stats.foam_mesh_index].vertices.size() == stats.foam_vertex_count);

    REQUIRE(river.Update(scene, 0.1f, &view));
    CHECK(stats.foam_updates == 2);
    CHECK(stats.foam_screen_particle_count > 0);
}

TEST_CASE("river scene rejects invalid dimensions") {
    auto scene = MakeRiverTerrainScene();
    parcel_hbg::RiverScene river;
    std::string error;
    CHECK_FALSE(river.Apply(scene, R"({"river":{"length":0.0,"width":10.0}})", &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(river.active());
    CHECK(scene.meshes.size() == 1);
}
