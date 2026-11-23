#include <catch_amalgamated.hpp>

#include "parcel/assets/animation_clip_asset.hpp"
#include "parcel/assets/ies_profile_asset.hpp"
#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/mesh_asset.hpp"
#include "parcel/assets/shader_library_asset.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/error.hpp"
#include "parcel/entt/scene_loader.hpp"
#include "parcel/hbg/asset_materializer.hpp"
#include "parcel/hbg/parcel_material_system.hpp"
#include "parcel/hbg/scene_render_bridge.hpp"
#include "parcel/hbg/settings_bridge.hpp"
#include "parcel/hbg/shader_library_bootstrap.hpp"
#include "parcel/scene/component_codec.hpp"
#include "parcel/scene/components.hpp"
#include "parcel/scene/scene_data.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using parcel::core::AssetGuid;
using parcel::core::ErrorCode;
using parcel::test::TempDir;
using parcel::test::ToUtf8;

namespace {

AssetGuid Guid(char value) {
    std::string text = "00112233445566778899aabbccddee0";
    text.push_back(value);
    return AssetGuid::FromHex(text).Value();
}

parcel::assets::TextureAsset MakeTexture(std::uint32_t width, std::uint32_t height) {
    parcel::assets::TextureAsset texture;
    texture.format = parcel::assets::TextureFormat::RGBA8;
    texture.sRGB = true;
    texture.mip_count = 1;
    parcel::assets::TextureMip mip;
    mip.width = width;
    mip.height = height;
    mip.row_pitch = width * 4;
    mip.data.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t index = 0; index < mip.data.size(); ++index) {
        mip.data[index] = static_cast<std::byte>(index & 0xff);
    }
    texture.mips.push_back(std::move(mip));
    return texture;
}

parcel::assets::TextureAsset MakeCookieTexture(std::uint32_t width, std::uint32_t height,
                                               std::uint8_t luminance) {
    auto texture = MakeTexture(width, height);
    for (std::size_t offset = 0; offset + 3 < texture.mips.front().data.size(); offset += 4) {
        texture.mips.front().data[offset] = static_cast<std::byte>(luminance);
        texture.mips.front().data[offset + 1] = static_cast<std::byte>(luminance);
        texture.mips.front().data[offset + 2] = static_cast<std::byte>(luminance);
        texture.mips.front().data[offset + 3] = std::byte{0xff};
    }
    return texture;
}

parcel::assets::AnimationClipAsset MakeAnimationClip(parcel::scene::SceneEntityId target) {
    parcel::assets::AnimationClipAsset clip;
    clip.name = "Move";
    clip.duration = 1.0f;
    parcel::assets::AnimationChannel channel;
    channel.target = target;
    channel.path = parcel::assets::AnimationPath::Translation;
    channel.interpolation = parcel::assets::AnimationInterpolation::Linear;
    channel.times = {0.0f, 1.0f};
    channel.values = {{0.0f, 0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f, 0.0f}};
    clip.channels.push_back(std::move(channel));
    return clip;
}

parcel::assets::AnimationClipAsset MakeMorphAnimationClip(parcel::scene::SceneEntityId target) {
    parcel::assets::AnimationClipAsset clip;
    clip.name = "Morph";
    clip.duration = 1.0f;
    parcel::assets::AnimationChannel channel;
    channel.target = target;
    channel.path = parcel::assets::AnimationPath::Weights;
    channel.interpolation = parcel::assets::AnimationInterpolation::Linear;
    channel.target_index = 0;
    channel.times = {0.0f, 1.0f};
    channel.values = {{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}};
    clip.channels.push_back(std::move(channel));
    return clip;
}

parcel::assets::IesProfileAsset MakeIesProfile() {
    parcel::assets::IesProfileAsset profile;
    profile.name = "test.ies";
    profile.vertical_angles = {0.0f, 45.0f, 90.0f};
    profile.horizontal_angles = {0.0f, 90.0f, 180.0f, 270.0f};
    profile.values = {
        1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.25f, 0.25f, 0.25f, 0.25f,
    };
    return profile;
}

parcel::assets::MeshAsset MakeMesh() {
    parcel::assets::MeshAsset mesh;
    mesh.name = "Triangle";
    mesh.has_normals = true;
    mesh.has_uv0 = true;
    mesh.vertices.resize(3);
    mesh.vertices[0].position = {0.0f, 0.0f, 0.0f};
    mesh.vertices[1].position = {1.0f, 0.0f, 0.0f};
    mesh.vertices[2].position = {0.0f, 1.0f, 0.0f};
    mesh.indices = {0, 1, 2};
    parcel::assets::SubMesh submesh;
    submesh.index_count = 3;
    submesh.material_index = 0;
    submesh.bounds.center = {0.5f, 0.5f, 0.0f};
    submesh.bounds.extents = {0.5f, 0.5f, 0.0f};
    mesh.sub_meshes.push_back(submesh);
    mesh.bounds = submesh.bounds;
    return mesh;
}

parcel::assets::MeshAsset MakeSkinnedMesh() {
    auto mesh = MakeMesh();
    mesh.skin_vertices.resize(mesh.vertices.size());
    for (std::size_t index = 0; index < mesh.skin_vertices.size(); ++index) {
        mesh.skin_vertices[index].joints = {0, 1, 0, 0};
        mesh.skin_vertices[index].weights = {0.75f, 0.25f, 0.0f, 0.0f};
    }
    return mesh;
}

parcel::assets::MeshAsset MakeMorphMesh() {
    auto mesh = MakeMesh();
    for (std::size_t target_index = 0; target_index < 2; ++target_index) {
        parcel::assets::MorphTarget target;
        target.name = "Target" + std::to_string(target_index);
        target.position_deltas.resize(mesh.vertices.size());
        for (std::size_t vertex = 0; vertex < mesh.vertices.size(); ++vertex) {
            target.position_deltas[vertex] = {static_cast<float>(target_index + 1) * 0.1f,
                                              static_cast<float>(vertex) * 0.05f, 0.0f};
        }
        mesh.morph_targets.push_back(std::move(target));
    }
    mesh.morph_weights = {0.0f, 0.0f};
    return mesh;
}

parcel::assets::MaterialAsset MakeMaterial(AssetGuid texture) {
    parcel::assets::MaterialAsset material;
    material.name = "Material";
    material.base_color_factor = {0.2f, 0.4f, 0.6f, 1.0f};
    material.metallic = 0.3f;
    material.roughness = 0.7f;
    material.base_color_texture.texture = texture;
    material.base_color_texture.uv_set = 1;
    material.base_color_texture.sampler.wrap_s = parcel::assets::TextureWrap::Repeat;
    material.base_color_texture.sampler.wrap_t = parcel::assets::TextureWrap::Mirror;
    material.base_color_texture.sampler.min_filter = parcel::assets::TextureFilter::Nearest;
    material.base_color_texture.sampler.mag_filter = parcel::assets::TextureFilter::Linear;
    material.base_color_texture.sampler.mip_filter = parcel::assets::TextureMipFilter::Nearest;
    material.normal_texture.texture = Guid('d');
    material.normal_texture.uv_set = 0;
    material.clear_coat = 0.5f;
    return material;
}

void AddEntity(parcel::scene::SceneData& scene, std::uint64_t id, std::uint64_t parent,
               const std::string& name) {
    parcel::scene::SceneEntityRecord record;
    record.id = {id};
    record.parent = {parent};
    record.name = name;
    scene.entities.push_back(std::move(record));
}

template <typename T>
void AddComponent(parcel::scene::SceneData& scene, parcel::scene::SceneEntityId owner,
                  const T& component) {
    const auto payload = parcel::scene::ComponentCodec<T>::Encode(component);
    REQUIRE(payload);
    auto* chunk = scene.FindComponent(parcel::scene::ComponentCodec<T>::type_id);
    if (chunk == nullptr) {
        parcel::scene::ComponentChunk created;
        created.type = parcel::scene::ComponentCodec<T>::type_id;
        created.schema_version = parcel::scene::ComponentCodec<T>::schema_version;
        scene.components.push_back(std::move(created));
        chunk = &scene.components.back();
    }
    chunk->owners.push_back(owner);
    chunk->payloads.push_back(payload.Value());
}

std::vector<std::byte> ReadBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    REQUIRE(size >= 0);
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(result.data()), size);
    REQUIRE(file);
    return result;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("hbg texture materializer preserves mips and formats") {
    auto texture = MakeTexture(4, 4);
    texture.mip_count = 2;
    parcel::assets::TextureMip second;
    second.width = 2;
    second.height = 2;
    second.row_pitch = 8;
    second.data.resize(16);
    texture.mips.push_back(std::move(second));

    auto materialized = parcel_hbg::MaterializeTexture(texture, "test");
    REQUIRE(materialized);
    const auto& value = materialized.Value();
    CHECK(value.desc.name == "test");
    CHECK(value.desc.width == 4);
    CHECK(value.desc.height == 4);
    CHECK(value.desc.mipCount == 2);
    CHECK(value.desc.format == hbg::gfx::TextureFormat::RGBA8);
    CHECK(value.desc.dataSize == 80);
    CHECK((value.desc.flags & BGFX_TEXTURE_SRGB) != 0);
    REQUIRE(value.storage);
    CHECK(value.storage->size() == 80);
    CHECK(value.desc.data == value.storage->data());

    parcel::assets::TextureAsset invalid = MakeTexture(4, 4);
    invalid.mips.front().data.resize(3);
    auto invalid_result = parcel_hbg::MaterializeTexture(invalid);
    REQUIRE_FALSE(invalid_result);
    CHECK(invalid_result.GetError().code == ErrorCode::InvalidArgument);

    parcel::assets::TextureAsset no_mips;
    auto no_mips_result = parcel_hbg::MaterializeTexture(no_mips);
    REQUIRE_FALSE(no_mips_result);
    CHECK(no_mips_result.GetError().code == ErrorCode::InvalidArgument);

    parcel::assets::TextureAsset bc7;
    bc7.format = parcel::assets::TextureFormat::BC7;
    bc7.mip_count = 1;
    parcel::assets::TextureMip bc7_mip;
    bc7_mip.width = 4;
    bc7_mip.height = 4;
    bc7_mip.data.resize(16, std::byte{0x7f});
    bc7.mips.push_back(std::move(bc7_mip));
    auto bc7_result = parcel_hbg::MaterializeTexture(bc7);
    REQUIRE(bc7_result);
    CHECK(bc7_result.Value().desc.format == hbg::gfx::TextureFormat::BC7);
    CHECK(bc7_result.Value().desc.dataSize == 16);

    parcel::assets::TextureAsset hdr;
    hdr.format = parcel::assets::TextureFormat::RGBA16F;
    hdr.mip_count = 1;
    parcel::assets::TextureMip hdr_mip;
    hdr_mip.width = 2;
    hdr_mip.height = 2;
    hdr_mip.data.resize(32, std::byte{0});
    hdr.mips.push_back(std::move(hdr_mip));
    auto hdr_result = parcel_hbg::MaterializeTexture(hdr);
    REQUIRE(hdr_result);
    CHECK(hdr_result.Value().desc.format == hbg::gfx::TextureFormat::RGBA16F);
    CHECK(hdr_result.Value().desc.dataSize == 32);

    bc7.mips.front().data.resize(8);
    auto bad_bc7 = parcel_hbg::MaterializeTexture(bc7);
    REQUIRE_FALSE(bad_bc7);
    CHECK(bad_bc7.GetError().code == ErrorCode::InvalidArgument);
}

TEST_CASE("hbg mesh and material mapping") {
    auto mesh = MakeMesh();
    auto converted_mesh = parcel_hbg::MaterializeMesh(mesh);
    REQUIRE(converted_mesh);
    CHECK(converted_mesh.Value().name == "Triangle");
    REQUIRE(converted_mesh.Value().vertices.size() == 3);
    CHECK(converted_mesh.Value().indices.size() == 3);
    CHECK(converted_mesh.Value().submeshes.size() == 1);
    CHECK(converted_mesh.Value().submeshes[0].materialIndex == 0);
    CHECK(converted_mesh.Value().bounds.min.x == Catch::Approx(0.0f));
    CHECK(converted_mesh.Value().bounds.max.y == Catch::Approx(1.0f));

    const AssetGuid texture_guid = Guid('a');
    auto material = MakeMaterial(texture_guid);
    const auto resolver = [&](AssetGuid guid) -> parcel::core::Result<int> {
        if (guid == texture_guid) {
            return 3;
        }
        if (guid == Guid('d')) {
            return 7;
        }
        return parcel::core::MakeError(ErrorCode::NotFound, "missing texture");
    };
    auto converted = parcel_hbg::MaterializeMaterial(material, resolver);
    REQUIRE(converted);
    CHECK(converted.Value().name == "Material");
    CHECK(converted.Value().baseColorTexture.imageIndex == 3);
    CHECK(converted.Value().baseColorTexture.uvSet == 1);
    CHECK(converted.Value().baseColorTexture.sampler.wrapS == hbg::asset::TextureWrap::Repeat);
    CHECK(converted.Value().baseColorTexture.sampler.wrapT == hbg::asset::TextureWrap::Mirror);
    CHECK(converted.Value().baseColorTexture.sampler.minFilter ==
          hbg::asset::TextureFilter::Nearest);
    CHECK(converted.Value().baseColorTexture.sampler.magFilter ==
          hbg::asset::TextureFilter::Linear);
    CHECK(converted.Value().baseColorTexture.sampler.mipFilter ==
          hbg::asset::TextureMipFilter::Nearest);
    CHECK(converted.Value().normalTexture.imageIndex == 7);
    CHECK(converted.Value().normalTexture.sampler == hbg::asset::TextureSampler{});
    CHECK(converted.Value().clearCoat == Catch::Approx(0.5f));

    auto key = parcel_hbg::MakeMaterialKey(converted.Value());
    CHECK(key.baseColorImage == 3);
    CHECK(key.normalImage == 7);
    CHECK(key.baseColorSampler ==
          hbg::asset::samplerStateKey(converted.Value().baseColorTexture.sampler));
    auto changed = converted.Value();
    changed.baseColorTexture.imageIndex = 4;
    CHECK_FALSE(parcel_hbg::MakeMaterialKey(changed) == key);
    changed = converted.Value();
    changed.baseColorTexture.sampler.wrapS = hbg::asset::TextureWrap::Clamp;
    CHECK_FALSE(parcel_hbg::MakeMaterialKey(changed) == key);

    material.normal_texture.texture = Guid('e');
    auto missing = parcel_hbg::MaterializeMaterial(material, resolver);
    REQUIRE_FALSE(missing);
    CHECK(missing.GetError().code == ErrorCode::NotFound);
}

TEST_CASE("hbg scene render bridge builds render scene from entt components") {
    const AssetGuid mesh_guid = Guid('a');
    const AssetGuid material_guid = Guid('b');
    const AssetGuid texture_guid = Guid('c');

    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "Root");
    AddEntity(scene_data, 2, 1, "Child");

    parcel::scene::TransformComponent transform;
    transform.position = {2.0f, 3.0f, 4.0f};
    transform.parent = {1};
    AddComponent(scene_data, {2}, transform);

    parcel::scene::CameraComponent camera;
    camera.yfov = 0.8f;
    camera.clear_color = {0.1f, 0.2f, 0.3f, 1.0f};
    camera.priority = 2;
    AddComponent(scene_data, {2}, camera);

    parcel::scene::LightComponent light;
    light.type = parcel::scene::LightType::Spot;
    light.intensity = 2.0f;
    AddComponent(scene_data, {2}, light);

    parcel::scene::MeshRendererComponent renderer;
    renderer.mesh = mesh_guid;
    renderer.materials = {material_guid};
    AddComponent(scene_data, {2}, renderer);

    parcel::scene::DecalComponent decal;
    decal.albedo = texture_guid;
    AddComponent(scene_data, {2}, decal);

    parcel::scene::ReflectionProbeComponent probe;
    probe.image = texture_guid;
    AddComponent(scene_data, {2}, probe);

    parcel::scene::LocalFogVolumeComponent fog;
    fog.mask = texture_guid;
    AddComponent(scene_data, {2}, fog);

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(mesh_guid, std::make_shared<parcel::assets::MeshAsset>(MakeMesh()));
    auto material = std::make_shared<parcel::assets::MaterialAsset>(MakeMaterial(texture_guid));
    material->normal_texture.texture = {};
    assets.emplace(material_guid, material);
    assets.emplace(texture_guid, std::make_shared<parcel::assets::TextureAsset>(MakeTexture(1, 1)));

    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;
    auto updated = bridge.Update(registry, render_scene);
    REQUIRE(updated);
    CHECK_FALSE(bridge.dirty());
    CHECK(render_scene.meshes.size() == 1);
    CHECK(render_scene.materials.size() == 1);
    CHECK(render_scene.images.size() == 1);
    CHECK(render_scene.objects.size() == 1);
    CHECK(render_scene.cameras.size() == 1);
    CHECK(render_scene.lights.size() == 1);
    CHECK(render_scene.decals.size() == 1);
    CHECK(render_scene.probes.size() == 1);
    CHECK(render_scene.localFogVolumes.size() == 1);
    REQUIRE(render_scene.objects.front().meshIndex == 0);
    CHECK(render_scene.meshes.front().submeshes.front().materialIndex == 0);
    REQUIRE(render_scene.nodes.size() == 2);
    CHECK(render_scene.nodes[0].name == "Root");
    CHECK(render_scene.nodes[0].parent == -1);
    CHECK(render_scene.nodes[1].name == "Child");
    CHECK(render_scene.nodes[1].parent == 0);
    CHECK(render_scene.nodes[1].translation.x == Catch::Approx(2.0f));
    CHECK(render_scene.nodes[1].translation.y == Catch::Approx(3.0f));
    CHECK(render_scene.nodes[1].translation.z == Catch::Approx(4.0f));
    CHECK(render_scene.nodes[1].baseTranslation.x == Catch::Approx(2.0f));
    CHECK(render_scene.objects.front().nodeIndex == 1);
    CHECK(render_scene.cameras.front().nodeIndex == 1);
    CHECK(render_scene.lights.front().nodeIndex == 1);
    CHECK(render_scene.objects.front().world[12] == Catch::Approx(2.0f));
    CHECK(render_scene.objects.front().world[13] == Catch::Approx(3.0f));
    CHECK(render_scene.objects.front().world[14] == Catch::Approx(4.0f));
    CHECK(render_scene.objects.front().worldBounds.valid());
    CHECK(render_scene.cameras.front().name == "Child");
    CHECK(render_scene.lights.front().type == hbg::render::LightType::Spot);
    CHECK(render_scene.decals.front().albedoImage == 0);
    CHECK(render_scene.probes.front().image == 0);
    CHECK(render_scene.localFogVolumes.front().maskImage == 0);
    REQUIRE(bridge.camera_states().size() == 1);
    REQUIRE(bridge.light_texture_states().size() == 1);
    CHECK(bridge.light_texture_states().front().cookie_image == -1);
    CHECK(bridge.camera_states().front().clear_color.x == Catch::Approx(0.1f));
    CHECK(bridge.camera_states().front().priority == 2);
    REQUIRE(bridge.image_guids().size() == 1);
    CHECK(bridge.image_guids().front() == texture_guid);

    auto cached = bridge.UpdateIfDirty(registry, render_scene);
    REQUIRE(cached);
    bridge.MarkDirty();
    auto rebuilt = bridge.UpdateIfDirty(registry, render_scene);
    REQUIRE(rebuilt);
    CHECK(render_scene.objects.size() == 1);

    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge preserves scene node order and hierarchy") {
    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 2, 0, "Root");
    AddEntity(scene_data, 1, 2, "Child");

    parcel::scene::TransformComponent root_transform;
    root_transform.position = {10.0f, 0.0f, 0.0f};
    AddComponent(scene_data, {2}, root_transform);

    parcel::scene::TransformComponent child_transform;
    child_transform.position = {0.0f, 2.0f, 0.0f};
    child_transform.rotation = {0.0f, 0.5f, 0.0f, 0.8660254f};
    child_transform.scale = {2.0f, 3.0f, 4.0f};
    child_transform.parent = {2};
    AddComponent(scene_data, {1}, child_transform);

    parcel::scene::CameraComponent camera;
    AddComponent(scene_data, {1}, camera);

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);

    parcel_hbg::SceneRenderBridge bridge;
    hbg::render::RenderScene render_scene;
    auto updated = bridge.Update(registry, render_scene);
    REQUIRE(updated);
    REQUIRE(render_scene.nodes.size() == 2);
    CHECK(render_scene.nodes[0].name == "Root");
    CHECK(render_scene.nodes[0].parent == -1);
    CHECK(render_scene.nodes[0].translation.x == Catch::Approx(10.0f));
    CHECK(render_scene.nodes[1].name == "Child");
    CHECK(render_scene.nodes[1].parent == 0);
    CHECK(render_scene.nodes[1].translation.y == Catch::Approx(2.0f));
    CHECK(render_scene.nodes[1].baseTranslation.y == Catch::Approx(2.0f));
    CHECK(render_scene.nodes[1].rotation.y == Catch::Approx(0.5f));
    CHECK(render_scene.nodes[1].baseRotation.w == Catch::Approx(0.8660254f));
    CHECK(render_scene.nodes[1].scale.x == Catch::Approx(2.0f));
    CHECK(render_scene.nodes[1].baseScale.z == Catch::Approx(4.0f));
    REQUIRE(render_scene.cameras.size() == 1);
    CHECK(render_scene.cameras.front().nodeIndex == 1);
    CHECK(render_scene.cameras.front().world[12] == Catch::Approx(10.0f));
    CHECK(render_scene.cameras.front().world[13] == Catch::Approx(2.0f));

    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge maps skin joints to scene nodes") {
    const AssetGuid mesh_guid = Guid('e');
    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "Root");
    AddEntity(scene_data, 2, 1, "JointA");
    AddEntity(scene_data, 3, 1, "JointB");
    AddEntity(scene_data, 4, 1, "Skinned");

    parcel::scene::TransformComponent root_transform;
    AddComponent(scene_data, {1}, root_transform);
    parcel::scene::TransformComponent joint_a;
    joint_a.position = {1.0f, 0.0f, 0.0f};
    AddComponent(scene_data, {2}, joint_a);
    parcel::scene::TransformComponent joint_b;
    joint_b.position = {0.0f, 1.0f, 0.0f};
    AddComponent(scene_data, {3}, joint_b);
    parcel::scene::TransformComponent mesh_transform;
    mesh_transform.parent = {1};
    AddComponent(scene_data, {4}, mesh_transform);

    parcel::scene::SkinComponent skin;
    skin.joints = {parcel::scene::SceneEntityId{2}, parcel::scene::SceneEntityId{3}};
    skin.inverse_bind_matrices.resize(2);
    skin.inverse_bind_matrices[0].m[12] = 0.25f;
    skin.inverse_bind_matrices[1].m[13] = 0.5f;
    AddComponent(scene_data, {4}, skin);

    parcel::scene::MeshRendererComponent renderer;
    renderer.mesh = mesh_guid;
    AddComponent(scene_data, {4}, renderer);

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(mesh_guid, std::make_shared<parcel::assets::MeshAsset>(MakeSkinnedMesh()));
    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;
    auto updated = bridge.Update(registry, render_scene);
    REQUIRE(updated);
    REQUIRE(render_scene.skins.size() == 1);
    const hbg::asset::Skin& converted = render_scene.skins.front();
    REQUIRE(converted.joints.size() == 2);
    CHECK(converted.joints[0] == 1);
    CHECK(converted.joints[1] == 2);
    CHECK(converted.inverseBindMatrices[0].values[12] == Catch::Approx(0.25f));
    CHECK(converted.inverseBindMatrices[1].values[13] == Catch::Approx(0.5f));
    REQUIRE(render_scene.meshes.size() == 1);
    REQUIRE(render_scene.meshes.front().skinVertices.size() == 3);
    REQUIRE(render_scene.meshes.front().skinInfluences.size() == 6);
    CHECK(render_scene.meshes.front().skinVertices.front().influenceOffset == 0);
    CHECK(render_scene.meshes.front().skinVertices.front().influenceCount == 2);
    CHECK(render_scene.meshes.front().skinInfluences[0].joint == 0);
    CHECK(render_scene.meshes.front().skinInfluences[1].joint == 1);
    CHECK(render_scene.meshes.front().skinInfluences[0].weight == Catch::Approx(0.75f));
    CHECK(render_scene.meshes.front().skinInfluences[1].weight == Catch::Approx(0.25f));
    REQUIRE(render_scene.objects.size() == 1);
    CHECK(render_scene.objects.front().skinIndex == 0);
    CHECK(render_scene.objects.front().nodeIndex == 3);
    CHECK_FALSE(render_scene.objects.front().staticShadowCaster);
    CHECK(render_scene.objects.front().worldBounds.valid());

    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge rejects invalid skin data") {
    const AssetGuid mesh_guid = Guid('d');
    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "Root");
    AddEntity(scene_data, 2, 1, "Skinned");
    parcel::scene::TransformComponent root_transform;
    AddComponent(scene_data, {1}, root_transform);
    parcel::scene::TransformComponent mesh_transform;
    mesh_transform.parent = {1};
    AddComponent(scene_data, {2}, mesh_transform);
    parcel::scene::MeshRendererComponent renderer;
    renderer.mesh = mesh_guid;
    AddComponent(scene_data, {2}, renderer);

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(mesh_guid, std::make_shared<parcel::assets::MeshAsset>(MakeSkinnedMesh()));
    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);
    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;
    auto missing = bridge.Update(registry, render_scene);
    REQUIRE_FALSE(missing);
    CHECK(missing.GetError().code == ErrorCode::Corrupt);
    REQUIRE(loader.Destroy(instance.Value()));

    parcel::scene::SkinComponent orphan_skin;
    orphan_skin.joints = {parcel::scene::SceneEntityId{99}};
    orphan_skin.inverse_bind_matrices.resize(1);
    AddComponent(scene_data, {2}, orphan_skin);
    instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);
    auto orphan = bridge.Update(registry, render_scene);
    REQUIRE_FALSE(orphan);
    CHECK(orphan.GetError().code == ErrorCode::Corrupt);
    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge maps morph targets and weights") {
    const AssetGuid mesh_guid = Guid('c');
    const AssetGuid clip_guid = Guid('b');
    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "Root");
    AddEntity(scene_data, 2, 1, "Morph");

    parcel::scene::AnimationPlayerComponent player;
    player.clips = {clip_guid};
    AddComponent(scene_data, {1}, player);

    parcel::scene::TransformComponent root_transform;
    AddComponent(scene_data, {1}, root_transform);
    parcel::scene::TransformComponent morph_transform;
    morph_transform.parent = {1};
    AddComponent(scene_data, {2}, morph_transform);

    parcel::scene::MorphComponent morph;
    morph.weights = {0.4f, 0.6f};
    AddComponent(scene_data, {2}, morph);
    parcel::scene::MeshRendererComponent renderer;
    renderer.mesh = mesh_guid;
    AddComponent(scene_data, {2}, renderer);

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(mesh_guid, std::make_shared<parcel::assets::MeshAsset>(MakeMorphMesh()));
    assets.emplace(clip_guid, std::make_shared<parcel::assets::AnimationClipAsset>(
                                  MakeMorphAnimationClip(parcel::scene::SceneEntityId{2})));
    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;
    auto updated = bridge.Update(registry, render_scene);
    REQUIRE(updated);
    REQUIRE(render_scene.meshes.size() == 1);
    REQUIRE(render_scene.meshes.front().morphTargets.size() == 2);
    CHECK(render_scene.meshes.front().morphTargets[0].name == "Target0");
    CHECK(render_scene.meshes.front().morphTargets[1].name == "Target1");
    CHECK(render_scene.meshes.front().morphTargets[1].positionDeltas[2].x == Catch::Approx(0.2f));
    REQUIRE(render_scene.objects.size() == 1);
    REQUIRE(render_scene.objects.front().morphWeights.size() == 2);
    CHECK(render_scene.objects.front().morphWeights[0] == Catch::Approx(0.4f));
    CHECK(render_scene.objects.front().morphWeights[1] == Catch::Approx(0.6f));
    REQUIRE(render_scene.nodes.size() == 2);
    REQUIRE(render_scene.nodes[1].weights.size() == 2);
    CHECK(render_scene.nodes[1].weights[0] == Catch::Approx(0.4f));
    CHECK(render_scene.nodes[1].baseWeights[1] == Catch::Approx(0.6f));
    REQUIRE(render_scene.animations.size() == 1);
    REQUIRE(render_scene.animations.front().channels.size() == 1);
    CHECK(render_scene.animations.front().channels.front().path ==
          hbg::asset::AnimationPath::Weights);
    CHECK(render_scene.animations.front().channels.front().targetIndex == 0);
    CHECK(render_scene.animations.front().channels.front().node == 1);
    CHECK(render_scene.animations.front().channels.front().values[1].x == Catch::Approx(1.0f));

    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge rejects invalid morph data") {
    const AssetGuid mesh_guid = Guid('a');
    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "Root");
    AddEntity(scene_data, 2, 1, "Morph");
    parcel::scene::TransformComponent root_transform;
    AddComponent(scene_data, {1}, root_transform);
    parcel::scene::TransformComponent morph_transform;
    morph_transform.parent = {1};
    AddComponent(scene_data, {2}, morph_transform);
    parcel::scene::MeshRendererComponent renderer;
    renderer.mesh = mesh_guid;
    AddComponent(scene_data, {2}, renderer);

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(mesh_guid, std::make_shared<parcel::assets::MeshAsset>(MakeMorphMesh()));
    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;

    parcel::scene::MorphComponent wrong_count;
    wrong_count.weights = {1.0f};
    AddComponent(scene_data, {2}, wrong_count);
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);
    auto invalid_weights = bridge.Update(registry, render_scene);
    REQUIRE_FALSE(invalid_weights);
    CHECK(invalid_weights.GetError().code == ErrorCode::Corrupt);
    REQUIRE(loader.Destroy(instance.Value()));

    scene_data.FindComponent(parcel::scene::kMorphComponentType)->payloads[0] =
        parcel::scene::ComponentCodec<parcel::scene::MorphComponent>::Encode(
            parcel::scene::MorphComponent{})
            .Value();
    instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);
    auto empty_weights = bridge.Update(registry, render_scene);
    REQUIRE_FALSE(empty_weights);
    CHECK(empty_weights.GetError().code == ErrorCode::Corrupt);
    REQUIRE(loader.Destroy(instance.Value()));

    assets[mesh_guid] = std::make_shared<parcel::assets::MeshAsset>(MakeMesh());
    parcel::scene::MorphComponent no_targets;
    no_targets.weights = {0.0f, 0.0f};
    scene_data.FindComponent(parcel::scene::kMorphComponentType)->payloads[0] =
        parcel::scene::ComponentCodec<parcel::scene::MorphComponent>::Encode(no_targets).Value();
    instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);
    auto no_morph_targets = bridge.Update(registry, render_scene);
    REQUIRE_FALSE(no_morph_targets);
    CHECK(no_morph_targets.GetError().code == ErrorCode::Corrupt);
    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge maps animation channels to scene nodes") {
    const AssetGuid clip_guid = Guid('f');
    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "Root");
    AddEntity(scene_data, 2, 1, "Child");

    parcel::scene::AnimationPlayerComponent player;
    player.clips = {clip_guid};
    AddComponent(scene_data, {1}, player);

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(clip_guid, std::make_shared<parcel::assets::AnimationClipAsset>(
                                  MakeAnimationClip(parcel::scene::SceneEntityId{2})));
    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;
    auto updated = bridge.Update(registry, render_scene);
    REQUIRE(updated);
    REQUIRE(render_scene.animations.size() == 1);
    const hbg::asset::AnimationClip& clip = render_scene.animations.front();
    CHECK(clip.name == "Move");
    REQUIRE(clip.channels.size() == 1);
    CHECK(clip.channels.front().node == 1);
    CHECK(clip.channels.front().path == hbg::asset::AnimationPath::Translation);
    CHECK(clip.channels.front().times.size() == 2);
    CHECK(clip.channels.front().values[1].x == Catch::Approx(2.0f));

    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge rejects animation targets without nodes") {
    const AssetGuid clip_guid = Guid('f');
    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "Root");
    parcel::scene::AnimationPlayerComponent player;
    player.clips = {clip_guid};
    AddComponent(scene_data, {1}, player);

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(clip_guid, std::make_shared<parcel::assets::AnimationClipAsset>(
                                  MakeAnimationClip(parcel::scene::SceneEntityId{99})));
    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;
    auto updated = bridge.Update(registry, render_scene);
    REQUIRE_FALSE(updated);
    CHECK(updated.GetError().code == ErrorCode::Corrupt);

    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge samples spot light cookie textures") {
    const AssetGuid cookie_guid = Guid('c');
    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "CookieLight");
    parcel::scene::TransformComponent transform;
    AddComponent(scene_data, {1}, transform);
    parcel::scene::LightComponent light;
    light.type = parcel::scene::LightType::Spot;
    light.cookie = cookie_guid;
    light.cookie_mode = parcel::scene::LightCookieMode::Clamp;
    light.cookie_type = parcel::scene::LightCookieType::Spot2D;
    AddComponent(scene_data, {1}, light);

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(cookie_guid,
                   std::make_shared<parcel::assets::TextureAsset>(MakeCookieTexture(2, 2, 128)));
    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;
    auto updated = bridge.Update(registry, render_scene);
    REQUIRE(updated);
    REQUIRE(render_scene.lights.size() == 1);
    const hbg::render::Light& converted = render_scene.lights.front();
    CHECK(converted.cookieMode == hbg::render::LightCookieMode::Clamp);
    CHECK(converted.cookieType == hbg::render::LightCookieType::Spot2D);
    CHECK(converted.cookieNativeWidth == 8);
    CHECK(converted.cookieNativeHeight == 8);
    CHECK(converted.cookieSamples[0] == Catch::Approx(128.0f / 255.0f).margin(1.0e-3f));
    REQUIRE(converted.cookieNative.size() == 64);
    CHECK(converted.cookieNative[0] == Catch::Approx(128.0f / 255.0f).margin(1.0e-3f));
    REQUIRE(bridge.light_texture_states().size() == 1);
    CHECK(bridge.light_texture_states().front().cookie_image == 0);
    REQUIRE(bridge.image_guids().size() == 1);
    CHECK(bridge.image_guids().front() == cookie_guid);

    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge samples point light cookies in octahedral space") {
    const AssetGuid cookie_guid = Guid('c');
    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "PointCookie");
    parcel::scene::TransformComponent transform;
    AddComponent(scene_data, {1}, transform);
    parcel::scene::LightComponent light;
    light.type = parcel::scene::LightType::Point;
    light.cookie = cookie_guid;
    light.cookie_mode = parcel::scene::LightCookieMode::Clamp;
    light.cookie_type = parcel::scene::LightCookieType::PointOctahedral;
    AddComponent(scene_data, {1}, light);

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(cookie_guid,
                   std::make_shared<parcel::assets::TextureAsset>(MakeCookieTexture(4, 2, 96)));
    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;
    auto updated = bridge.Update(registry, render_scene);
    REQUIRE(updated);
    REQUIRE(render_scene.lights.size() == 1);
    const hbg::render::Light& converted = render_scene.lights.front();
    CHECK(converted.cookieType == hbg::render::LightCookieType::PointOctahedral);
    CHECK(converted.cookieNativeWidth == 8);
    CHECK(converted.cookieNativeHeight == 8);
    CHECK(converted.cookieSamples[0] == Catch::Approx(96.0f / 255.0f).margin(1.0e-3f));

    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge fills spot light IES samples") {
    const AssetGuid ies_guid = Guid('e');
    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "IesSpot");
    parcel::scene::TransformComponent transform;
    AddComponent(scene_data, {1}, transform);
    parcel::scene::LightComponent light;
    light.type = parcel::scene::LightType::Spot;
    light.ies = ies_guid;
    light.has_ies = true;
    AddComponent(scene_data, {1}, light);

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(ies_guid, std::make_shared<parcel::assets::IesProfileAsset>(MakeIesProfile()));
    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;
    auto updated = bridge.Update(registry, render_scene);
    REQUIRE(updated);
    REQUIRE(render_scene.lights.size() == 1);
    const hbg::render::Light& converted = render_scene.lights.front();
    CHECK(converted.hasIes);
    CHECK(converted.iesSamples[0] == Catch::Approx(1.0f));
    CHECK(converted.iesSamples[4] == Catch::Approx(0.4643f).margin(1.0e-3f));
    CHECK(converted.iesSamples[7] == Catch::Approx(0.25f));
    CHECK(converted.iesCut == Catch::Approx(0.0f).margin(1.0e-6f));
    REQUIRE(bridge.light_texture_states().size() == 1);
    CHECK(bridge.light_texture_states().front().ies_image == -1);

    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge modulates point cookie native data with IES") {
    const AssetGuid cookie_guid = Guid('c');
    const AssetGuid ies_guid = Guid('e');
    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "IesPoint");
    parcel::scene::TransformComponent transform;
    AddComponent(scene_data, {1}, transform);
    parcel::scene::LightComponent light;
    light.type = parcel::scene::LightType::Point;
    light.cookie = cookie_guid;
    light.cookie_mode = parcel::scene::LightCookieMode::Clamp;
    light.cookie_type = parcel::scene::LightCookieType::PointOctahedral;
    light.ies = ies_guid;
    light.has_ies = true;
    AddComponent(scene_data, {1}, light);

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);

    parcel::assets::IesProfileAsset profile;
    profile.name = "point.ies";
    profile.vertical_angles = {0.0f, 90.0f};
    profile.horizontal_angles = {0.0f, 180.0f};
    profile.values = {0.5f, 1.0f, 0.5f, 0.5f};

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(cookie_guid,
                   std::make_shared<parcel::assets::TextureAsset>(MakeCookieTexture(2, 2, 128)));
    assets.emplace(ies_guid, std::make_shared<parcel::assets::IesProfileAsset>(profile));
    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;
    auto updated = bridge.Update(registry, render_scene);
    REQUIRE(updated);
    REQUIRE(render_scene.lights.size() == 1);
    const hbg::render::Light& converted = render_scene.lights.front();
    CHECK(converted.hasIes);
    CHECK(converted.cookieType == hbg::render::LightCookieType::PointOctahedral);
    CHECK(converted.cookieNativeWidth == 8);
    REQUIRE(converted.cookieNative.size() == 64);
    CHECK(converted.cookieSamples[0] == Catch::Approx(128.0f / 255.0f * 0.5f).margin(1.0e-3f));
    CHECK(converted.cookieNative[0] == Catch::Approx(128.0f / 255.0f * 0.5f).margin(1.0e-3f));

    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge rejects compressed light cookies") {
    const AssetGuid cookie_guid = Guid('c');
    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "CompressedCookie");
    parcel::scene::TransformComponent transform;
    AddComponent(scene_data, {1}, transform);
    parcel::scene::LightComponent light;
    light.type = parcel::scene::LightType::Spot;
    light.cookie = cookie_guid;
    AddComponent(scene_data, {1}, light);

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);

    auto texture = MakeTexture(4, 4);
    texture.format = parcel::assets::TextureFormat::BC7;
    texture.mips.front().row_pitch = 16;
    texture.mips.front().data.resize(16);
    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(cookie_guid, std::make_shared<parcel::assets::TextureAsset>(std::move(texture)));
    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;
    auto updated = bridge.Update(registry, render_scene);
    REQUIRE_FALSE(updated);
    CHECK(updated.GetError().code == ErrorCode::Unsupported);

    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg scene render bridge reports missing referenced assets") {
    const AssetGuid mesh_guid = Guid('a');
    const AssetGuid material_guid = Guid('b');
    const AssetGuid texture_guid = Guid('c');

    parcel::scene::SceneData scene_data;
    AddEntity(scene_data, 1, 0, "Root");
    parcel::scene::TransformComponent transform;
    AddComponent(scene_data, {1}, transform);
    parcel::scene::MeshRendererComponent renderer;
    renderer.mesh = mesh_guid;
    renderer.materials = {material_guid};
    AddComponent(scene_data, {1}, renderer);

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    REQUIRE(instance);

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(mesh_guid, std::make_shared<parcel::assets::MeshAsset>(MakeMesh()));
    auto material = std::make_shared<parcel::assets::MaterialAsset>(MakeMaterial(texture_guid));
    assets.emplace(material_guid, material);

    auto resolve =
        [&](AssetGuid guid) -> parcel::core::Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return parcel::core::MakeError(ErrorCode::NotFound, "missing asset");
        }
        return found->second;
    };

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene render_scene;
    auto updated = bridge.Update(registry, render_scene);
    REQUIRE_FALSE(updated);
    CHECK(updated.GetError().code == ErrorCode::NotFound);
    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("hbg settings bridge maps pipeline frame postprocess shadow and fog") {
    parcel::assets::PipelineSettingsAsset pipeline_asset;
    pipeline_asset.settings.supported_lit_shader_mode =
        parcel::assets::SupportedLitShaderMode::Both;
    pipeline_asset.settings.support_volumetrics = true;
    pipeline_asset.settings.cookie_atlas_size = 2048;
    auto pipeline = parcel_hbg::SettingsBridge::MapPipeline(pipeline_asset);
    REQUIRE(pipeline);
    CHECK(pipeline.Value().supportedLitShaderMode == hbg::render::SupportedLitShaderMode::Both);
    CHECK(pipeline.Value().supportVolumetrics);
    CHECK(pipeline.Value().cookieAtlasSize == 2048);

    parcel::assets::FrameSettingsAsset frame_asset;
    frame_asset.settings.bit_data[0] = 0x5;
    frame_asset.settings.bit_data[1] = 0x8000000000000000ULL;
    frame_asset.settings.lod_bias = 1.5f;
    frame_asset.settings.material_quality = parcel::assets::MaterialQuality::Medium;
    auto frame = parcel_hbg::SettingsBridge::MapFrame(frame_asset);
    REQUIRE(frame);
    CHECK(frame.Value().bitDatas.low() == 0x5);
    CHECK(frame.Value().bitDatas.high() == 0x8000000000000000ULL);
    CHECK(frame.Value().lodBias == Catch::Approx(1.5f));
    CHECK(frame.Value().materialQuality == hbg::render::MaterialQuality::Medium);

    parcel::assets::PostProcessProfileAsset post_asset;
    post_asset.profile.name = "custom";
    post_asset.profile.settings.exposure_mode = parcel::assets::ExposureMode::Automatic;
    post_asset.profile.settings.bloom.enabled = true;
    post_asset.profile.settings.bloom.intensity = 0.8f;
    post_asset.profile.settings.color_grading.enabled = true;
    post_asset.profile.settings.color_grading.contrast = 0.2f;
    auto post = parcel_hbg::SettingsBridge::MapPostProcess(post_asset);
    REQUIRE(post);
    CHECK(post.Value().name == "custom");
    CHECK(post.Value().settings.exposureMode == hbg::render::ExposureMode::Automatic);
    CHECK(post.Value().settings.bloom.enabled);
    CHECK(post.Value().settings.bloom.intensity == Catch::Approx(0.8f));
    CHECK(post.Value().settings.colorGrading.contrast == Catch::Approx(0.2f));

    parcel::assets::ShadowSettingsAsset shadow_asset;
    shadow_asset.settings.cascade_count = 3;
    shadow_asset.settings.max_requests = 32;
    shadow_asset.settings.contact_shadows.enabled = true;
    auto shadow = parcel_hbg::SettingsBridge::MapShadows(shadow_asset);
    REQUIRE(shadow);
    CHECK(shadow.Value().cascadeCount == 3);
    CHECK(shadow.Value().maxRequests == 32);
    CHECK(shadow.Value().contactShadows.enabled);

    parcel::assets::VolumetricFogProfileAsset fog_asset;
    fog_asset.settings.enabled = true;
    fog_asset.settings.step_count = 32;
    fog_asset.settings.mean_free_path = 12.0f;
    auto fog = parcel_hbg::SettingsBridge::MapVolumetricFog(fog_asset);
    REQUIRE(fog);
    CHECK(fog.Value().enabled);
    CHECK(fog.Value().stepCount == 32);
    CHECK(fog.Value().meanFreePath == Catch::Approx(12.0f));

    pipeline_asset.settings.light_list_capacity = 0;
    CHECK_FALSE(parcel_hbg::SettingsBridge::MapPipeline(pipeline_asset));
    shadow_asset.settings.cascade_count = 5;
    CHECK_FALSE(parcel_hbg::SettingsBridge::MapShadows(shadow_asset));
}

TEST_CASE("hbg shader bootstrap materializes manifest and binaries with cleanup") {
    TempDir directory("hbg_shader");
    parcel::assets::ShaderLibraryAsset library;
    library.name = "TestShaders";
    library.material_variant = "lit";
    library.pipeline_key = "forward";

    parcel::assets::ShaderBackendLibrary backend;
    backend.backend = parcel::assets::ShaderBackend::Dxbc;
    parcel::assets::ShaderPassBinary vertex;
    vertex.pass = "forward";
    vertex.stage = parcel::assets::ShaderStage::Vertex;
    vertex.data = {std::byte{1}, std::byte{2}, std::byte{3}};
    backend.passes.push_back(vertex);
    parcel::assets::ShaderPassBinary fragment = vertex;
    fragment.stage = parcel::assets::ShaderStage::Fragment;
    fragment.data = {std::byte{4}, std::byte{5}};
    backend.passes.push_back(fragment);
    library.backends.push_back(backend);

    auto hash = parcel_hbg::ShaderLibraryBootstrap::ContentHash(library);
    REQUIRE(hash);
    CHECK(hash.Value().size() == 64);
    auto same_hash = parcel_hbg::ShaderLibraryBootstrap::ContentHash(library);
    REQUIRE(same_hash);
    CHECK(same_hash.Value() == hash.Value());
    parcel::assets::ShaderLibraryAsset changed = library;
    changed.backends.front().passes.front().data[0] = std::byte{9};
    auto changed_hash = parcel_hbg::ShaderLibraryBootstrap::ContentHash(changed);
    REQUIRE(changed_hash);
    CHECK(changed_hash.Value() != hash.Value());

    parcel_hbg::ShaderMaterializeOptions options;
    options.cache_root = directory.Path();
    options.package = "game";
    auto materialized = parcel_hbg::ShaderLibraryBootstrap::Materialize(library, options);
    REQUIRE(materialized);
    CHECK(std::filesystem::exists(materialized.Value().root / "manifest.json"));
    CHECK(materialized.Value().files.size() == 3);
    for (const auto& file : materialized.Value().files) {
        CHECK(std::filesystem::exists(file));
    }
    const std::string manifest = ReadText(materialized.Value().root / "manifest.json");
    CHECK(manifest.find("\"dxbc\"") != std::string::npos);
    CHECK(manifest.find("\"fragments\"") != std::string::npos);
    CHECK(manifest.find("forward.vs.bin") != std::string::npos);
    const auto vertex_bytes =
        ReadBytes(materialized.Value().root / "build" / "dxbc" / "forward.vs.bin");
    CHECK(vertex_bytes.size() == 3);

    const std::filesystem::path old =
        options.cache_root / "parcel" / "game" / "shader_library" / "old";
    std::filesystem::create_directories(old);
    auto removed = parcel_hbg::ShaderLibraryBootstrap::CleanupOldVersions(
        options, materialized.Value().content_hash);
    REQUIRE(removed);
    CHECK(removed.Value() == 1);
    CHECK_FALSE(std::filesystem::exists(old));
    CHECK(std::filesystem::exists(materialized.Value().root));

    struct FakeResources {};
    struct FakeLibrary {
        bool init(FakeResources&, const std::filesystem::path& root) {
            initialized = true;
            root_seen = root;
            return true;
        }
        bool initialized = false;
        std::filesystem::path root_seen;
    };
    FakeResources resources;
    FakeLibrary fake;
    auto initialized =
        parcel_hbg::ShaderLibraryBootstrap::Initialize(fake, resources, materialized.Value());
    REQUIRE(initialized);
    CHECK(fake.initialized);
    CHECK(fake.root_seen == materialized.Value().root);

    parcel::assets::ShaderLibraryAsset empty;
    auto empty_result = parcel_hbg::ShaderLibraryBootstrap::Materialize(empty, options);
    REQUIRE_FALSE(empty_result);
    CHECK(empty_result.GetError().code == ErrorCode::InvalidArgument);

    parcel::assets::ShaderLibraryAsset unsafe;
    parcel::assets::ShaderBackendLibrary unsafe_backend;
    parcel::assets::ShaderPassBinary unsafe_pass;
    unsafe_pass.pass = "../escape";
    unsafe_pass.data = {std::byte{1}};
    unsafe_backend.passes.push_back(unsafe_pass);
    unsafe.backends.push_back(unsafe_backend);
    auto unsafe_result = parcel_hbg::ShaderLibraryBootstrap::Materialize(unsafe, options);
    REQUIRE_FALSE(unsafe_result);
    CHECK(unsafe_result.GetError().code == ErrorCode::InvalidArgument);
}

TEST_CASE("hbg gpu material uniforms and variants match material system mapping") {
    hbg::asset::Material material;
    material.metallic = 0.25f;
    material.roughness = 0.75f;
    material.normalScale = 2.0f;
    material.alphaCutoff = 0.4f;
    material.emissiveFactor = hbg::Vec3{0.1f, 0.2f, 0.3f};
    material.emissiveStrength = 4.0f;
    material.occlusionStrength = 0.6f;
    material.clearCoat = 0.2f;
    material.clearCoatRoughness = 0.1f;
    material.transmission = 0.3f;
    material.thickness = 0.5f;
    material.anisotropy = 0.4f;
    material.anisotropyRotation = 0.6f;
    material.ior = 1.4f;
    material.iridescence = 0.7f;
    material.iridescenceThickness = 0.8f;
    material.subsurfaceMask = 0.9f;
    material.clearCoatNormalScale = 1.1f;
    material.features = hbg::asset::MaterialFeatureClearCoat | hbg::asset::MaterialFeatureIor;
    material.alphaMode = hbg::asset::AlphaMode::Mask;
    material.doubleSided = true;
    material.normalTexture.imageIndex = 2;
    material.metallicRoughnessTexture.imageIndex = 3;

    const auto uniforms = parcel_hbg::MakeGpuMaterialUniforms(material);
    CHECK(uniforms.metallicRoughness.x == Catch::Approx(0.25f));
    CHECK(uniforms.metallicRoughness.y == Catch::Approx(0.75f));
    CHECK(uniforms.metallicRoughness.z == Catch::Approx(2.0f));
    CHECK(uniforms.metallicRoughness.w == Catch::Approx(0.4f));
    CHECK(uniforms.emissiveFactor.x == Catch::Approx(0.4f));
    CHECK(uniforms.emissiveFactor.w == Catch::Approx(0.6f));
    CHECK(uniforms.advanced0.x == Catch::Approx(0.2f));
    CHECK(uniforms.advanced1.z == Catch::Approx(1.4f));
    CHECK(uniforms.advanced2.w == Catch::Approx(1.1f));

    const std::uint64_t variant = parcel_hbg::MakeGpuMaterialVariant(material);
    CHECK((variant & hbg::shader::MaterialVariantNormalMap) != 0);
    CHECK((variant & hbg::shader::MaterialVariantMaskMap) != 0);
    CHECK((variant & hbg::shader::MaterialVariantAlphaClip) != 0);
    CHECK((variant & hbg::shader::MaterialVariantDoubleSided) != 0);
    CHECK((variant & hbg::shader::MaterialVariantTransparent) == 0);
}
