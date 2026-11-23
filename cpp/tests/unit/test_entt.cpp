#include <catch_amalgamated.hpp>

#include "parcel/entt/scene_loader.hpp"
#include "parcel/scene/component_codec.hpp"
#include "parcel/scene/components.hpp"
#include "parcel/scene/scene_data.hpp"

#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using parcel::core::AssetGuid;
using parcel::scene::AnimationPlayerComponent;
using parcel::scene::CameraComponent;
using parcel::scene::ComponentChunk;
using parcel::scene::ComponentCodec;
using parcel::scene::DecalComponent;
using parcel::scene::EntityId;
using parcel::scene::LightComponent;
using parcel::scene::LocalFogVolumeComponent;
using parcel::scene::MeshRendererComponent;
using parcel::scene::MorphComponent;
using parcel::scene::ReflectionProbeComponent;
using parcel::scene::SceneData;
using parcel::scene::SceneEntityId;
using parcel::scene::SceneEntityRecord;
using parcel::scene::SkinComponent;
using parcel::scene::TransformComponent;

namespace {

struct CustomValue {
    std::uint8_t value = 0;
};

AssetGuid Guid(char value) {
    std::string text = "00112233445566778899aabbccddee0";
    text.push_back(value);
    return AssetGuid::FromHex(text).Value();
}

void AddEntity(SceneData& scene, EntityId id, EntityId parent, const std::string& name) {
    SceneEntityRecord record;
    record.id = {id};
    record.parent = {parent};
    record.name = name;
    scene.entities.push_back(std::move(record));
}

template <typename T> void AddComponent(SceneData& scene, SceneEntityId owner, const T& component) {
    const auto payload = ComponentCodec<T>::Encode(component);
    REQUIRE(payload);
    ComponentChunk* chunk = scene.FindComponent(ComponentCodec<T>::type_id);
    if (chunk == nullptr) {
        ComponentChunk created;
        created.type = ComponentCodec<T>::type_id;
        created.schema_version = ComponentCodec<T>::schema_version;
        scene.components.push_back(std::move(created));
        chunk = &scene.components.back();
    }
    chunk->owners.push_back(owner);
    chunk->payloads.push_back(payload.Value());
}

SceneData MakeScene() {
    SceneData scene;
    AddEntity(scene, 1, 0, "Root");
    AddEntity(scene, 2, 1, "Child");

    TransformComponent transform;
    transform.position = {1.0f, 2.0f, 3.0f};
    transform.rotation = {0.0f, 0.25f, 0.0f, 0.9682458f};
    transform.scale = {2.0f, 2.0f, 2.0f};
    transform.parent = {1};
    AddComponent(scene, {2}, transform);

    CameraComponent camera;
    camera.projection = CameraComponent::Projection::Orthographic;
    camera.yfov = 0.75f;
    camera.aspect = 1.5f;
    camera.culling_mask = 0x00ff00ffU;
    camera.priority = 3;
    AddComponent(scene, {2}, camera);

    MeshRendererComponent renderer;
    renderer.mesh = Guid('a');
    renderer.materials = {Guid('b'), Guid('c')};
    renderer.submesh_materials = {1, 0};
    renderer.sorting_layer = 4;
    AddComponent(scene, {2}, renderer);

    LightComponent light;
    light.type = parcel::scene::LightType::Spot;
    light.intensity = 7.0f;
    light.has_ies = true;
    AddComponent(scene, {2}, light);

    DecalComponent decal;
    decal.albedo = Guid('d');
    decal.opacity = 0.25f;
    AddComponent(scene, {2}, decal);

    ReflectionProbeComponent probe;
    probe.intensity = 1.5f;
    AddComponent(scene, {2}, probe);

    LocalFogVolumeComponent fog;
    fog.mean_free_path = 42.0f;
    AddComponent(scene, {2}, fog);

    AnimationPlayerComponent animation;
    animation.clips = {Guid('e')};
    AddComponent(scene, {2}, animation);

    SkinComponent skin;
    skin.joints = {SceneEntityId{1}, SceneEntityId{2}};
    skin.inverse_bind_matrices.resize(2);
    skin.inverse_bind_matrices[0].m[12] = 3.0f;
    AddComponent(scene, {2}, skin);

    MorphComponent morph;
    morph.weights = {0.25f, 0.75f};
    AddComponent(scene, {2}, morph);
    return scene;
}

::entt::entity FindEntity(const ::entt::registry& registry, EntityId id) {
    for (const auto entity : registry.view<const parcel_entt::SceneEntityComponent>()) {
        const auto& tag = registry.get<const parcel_entt::SceneEntityComponent>(entity);
        if (tag.id.value == id) {
            return entity;
        }
    }
    return ::entt::null;
}

} // namespace

TEST_CASE("entt scene loader instantiates hierarchy and standard components") {
    const SceneData scene = MakeScene();
    ::entt::registry registry;
    const ::entt::entity existing = registry.create();

    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene, &registry);
    REQUIRE(instance);
    CHECK(instance.Value().registry == &registry);
    REQUIRE(instance.Value().entities.size() == 2);
    CHECK(instance.Value().entities[0] == SceneEntityId{1});
    CHECK(instance.Value().entities[1] == SceneEntityId{2});
    CHECK(instance.Value().context);

    const ::entt::entity root = FindEntity(registry, 1);
    const ::entt::entity child = FindEntity(registry, 2);
    REQUIRE(registry.valid(root));
    REQUIRE(registry.valid(child));
    CHECK(registry.get<parcel_entt::SceneEntityComponent>(root).name == "Root");
    CHECK(registry.get<parcel_entt::SceneEntityComponent>(child).name == "Child");

    const auto& transform = registry.get<TransformComponent>(child);
    CHECK(transform.position.x == Catch::Approx(1.0f));
    CHECK(transform.scale.x == Catch::Approx(2.0f));
    CHECK(transform.parent == SceneEntityId{1});
    const auto& parent = registry.get<parcel_entt::ParentEntityComponent>(child);
    CHECK(parent.id == SceneEntityId{1});
    CHECK(parent.entity == root);

    const auto& camera = registry.get<CameraComponent>(child);
    CHECK(camera.projection == CameraComponent::Projection::Orthographic);
    CHECK(camera.yfov == Catch::Approx(0.75f));
    CHECK(camera.culling_mask == 0x00ff00ffU);
    CHECK(camera.priority == 3);

    const auto& renderer = registry.get<MeshRendererComponent>(child);
    CHECK(renderer.mesh == Guid('a'));
    CHECK(renderer.materials.size() == 2);
    CHECK(renderer.sorting_layer == 4);
    CHECK(registry.get<LightComponent>(child).type == parcel::scene::LightType::Spot);
    CHECK(registry.get<DecalComponent>(child).opacity == Catch::Approx(0.25f));
    CHECK(registry.get<ReflectionProbeComponent>(child).intensity == Catch::Approx(1.5f));
    CHECK(registry.get<LocalFogVolumeComponent>(child).mean_free_path == Catch::Approx(42.0f));
    CHECK(registry.get<AnimationPlayerComponent>(child).clips.size() == 1);
    const auto& skin = registry.get<SkinComponent>(child);
    REQUIRE(skin.joints.size() == 2);
    CHECK(skin.joints[0] == SceneEntityId{1});
    CHECK(skin.inverse_bind_matrices[0].m[12] == Catch::Approx(3.0f));
    const auto& morph = registry.get<MorphComponent>(child);
    REQUIRE(morph.weights.size() == 2);
    CHECK(morph.weights[0] == Catch::Approx(0.25f));
    CHECK(morph.weights[1] == Catch::Approx(0.75f));

    auto destroyed = loader.Destroy(instance.Value());
    REQUIRE(destroyed);
    CHECK_FALSE(registry.valid(child));
    CHECK_FALSE(registry.valid(root));
    CHECK(registry.valid(existing));
    CHECK(instance.Value().entities.empty());
    CHECK_FALSE(instance.Value().context);
    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("entt scene loader rolls back after custom component emplace failure") {
    SceneData scene;
    AddEntity(scene, 1, 0, "Failure");

    parcel_entt::EnttComponentRegistry components;
    auto registered = components.Register(
        100, "Failure", 1,
        +[](::entt::registry&, ::entt::entity,
            std::span<const std::byte>) -> parcel::core::Result<void> {
            return parcel::core::MakeError(parcel::core::ErrorCode::Internal, "emplace failed");
        });
    REQUIRE(registered);
    ComponentChunk chunk;
    chunk.type = 100;
    chunk.schema_version = 1;
    chunk.owners.push_back(SceneEntityId{1});
    chunk.payloads.push_back(std::vector<std::byte>{std::byte{1}});
    scene.components.push_back(std::move(chunk));

    ::entt::registry registry;
    const ::entt::entity existing = registry.create();
    parcel_entt::EnttSceneLoader loader(&components);
    auto instance = loader.Instantiate(scene, &registry);
    REQUIRE_FALSE(instance);
    CHECK(instance.GetError().code == parcel::core::ErrorCode::Internal);
    CHECK(registry.valid(existing));
    CHECK(registry.view<parcel_entt::SceneEntityComponent>().size() == 0);
}

TEST_CASE("entt scene loader validates parents and unsupported components before creation") {
    SceneData scene;
    AddEntity(scene, 1, 0, "Root");
    AddEntity(scene, 2, 99, "Orphan");
    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto invalid_parent = loader.Instantiate(scene, &registry);
    REQUIRE_FALSE(invalid_parent);
    CHECK(invalid_parent.GetError().code == parcel::core::ErrorCode::Corrupt);
    CHECK(registry.view<parcel_entt::SceneEntityComponent>().size() == 0);

    SceneData unsupported;
    AddEntity(unsupported, 1, 0, "Root");
    ComponentChunk chunk;
    chunk.type = 999;
    chunk.schema_version = 1;
    chunk.owners.push_back(SceneEntityId{1});
    chunk.payloads.push_back(std::vector<std::byte>{std::byte{1}});
    unsupported.components.push_back(std::move(chunk));
    auto unknown = loader.Instantiate(unsupported, &registry);
    REQUIRE_FALSE(unknown);
    CHECK(unknown.GetError().code == parcel::core::ErrorCode::Unsupported);
    CHECK(registry.view<parcel_entt::SceneEntityComponent>().size() == 0);
}

TEST_CASE("entt scene loader supports custom component registration") {
    SceneData scene;
    AddEntity(scene, 1, 0, "Custom");

    parcel_entt::EnttComponentRegistry components;
    auto registered = components.Register(
        200, "CustomValue", 1,
        +[](::entt::registry& registry, ::entt::entity entity,
            std::span<const std::byte> payload) -> parcel::core::Result<void> {
            if (payload.size() != 1) {
                return parcel::core::MakeError(parcel::core::ErrorCode::Corrupt,
                                               "custom payload is invalid");
            }
            registry.emplace_or_replace<CustomValue>(
                entity, CustomValue{static_cast<std::uint8_t>(payload[0])});
            return {};
        });
    REQUIRE(registered);
    ComponentChunk chunk;
    chunk.type = 200;
    chunk.schema_version = 1;
    chunk.owners.push_back(SceneEntityId{1});
    chunk.payloads.push_back(std::vector<std::byte>{std::byte{42}});
    scene.components.push_back(std::move(chunk));

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader(&components);
    auto instance = loader.Instantiate(scene, &registry);
    REQUIRE(instance);
    const ::entt::entity entity = FindEntity(registry, 1);
    REQUIRE(registry.valid(entity));
    CHECK(registry.get<CustomValue>(entity).value == 42);
    REQUIRE(loader.Destroy(instance.Value()));
}

TEST_CASE("entt scene loader requires a registry") {
    SceneData scene;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene, nullptr);
    REQUIRE_FALSE(instance);
    CHECK(instance.GetError().code == parcel::core::ErrorCode::InvalidArgument);
}

TEST_CASE("entt scene loader accepts SceneAsset and empty scenes") {
    parcel::assets::SceneAsset asset;
    asset.scene = MakeScene();
    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(asset, &registry);
    REQUIRE(instance);
    CHECK(instance.Value().entities.size() == 2);
    REQUIRE(loader.Destroy(instance.Value()));

    parcel::scene::SceneData empty;
    auto empty_instance = loader.Instantiate(empty, &registry);
    REQUIRE(empty_instance);
    CHECK(empty_instance.Value().entities.empty());
    CHECK(empty_instance.Value().context);
    REQUIRE(loader.Destroy(empty_instance.Value()));
}
