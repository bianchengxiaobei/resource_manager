#include <catch_amalgamated.hpp>

#include "parcel/scene/component_codec.hpp"
#include "parcel/scene/scene_data.hpp"
#include "parcel/scene/scene_loader.hpp"

#include <limits>
#include <string>
#include <vector>

using parcel::core::AssetGuid;
using parcel::scene::AnimationPlayerComponent;
using parcel::scene::CameraComponent;
using parcel::scene::ColliderComponent;
using parcel::scene::ComponentChunk;
using parcel::scene::ComponentCodec;
using parcel::scene::ComponentRegistry;
using parcel::scene::LightComponent;
using parcel::scene::MorphComponent;
using parcel::scene::SceneData;
using parcel::scene::SceneEntityId;
using parcel::scene::SceneEntityRecord;
using parcel::scene::SkinComponent;
using parcel::scene::StandardComponentRegistry;
using parcel::scene::TransformComponent;

namespace {

AssetGuid Guid(char value) {
    std::string text = "00112233445566778899aabbccddee0";
    text.push_back(value);
    return AssetGuid::FromHex(text).Value();
}

template <typename T> void CheckComponentRoundTrip(const T& value) {
    const auto encoded = ComponentCodec<T>::Encode(value);
    REQUIRE(encoded);
    const auto decoded = ComponentCodec<T>::Decode(encoded.Value());
    REQUIRE(decoded);
    CHECK(decoded.Value() == value);
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
    scene.pipeline_settings = Guid('1');
    scene.frame_settings = Guid('2');
    scene.post_process_profile = Guid('3');
    scene.shadow_settings = Guid('4');
    scene.volumetric_fog_profile = Guid('5');

    SceneEntityRecord root;
    root.id = {1};
    root.name = "Root";
    root.transform.position = {1.0f, 2.0f, 3.0f};
    scene.entities.push_back(root);

    SceneEntityRecord child;
    child.id = {2};
    child.parent = {1};
    child.name = "Child";
    child.transform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    scene.entities.push_back(child);

    parcel::scene::TransformComponent transform;
    transform.position = {4.0f, 5.0f, 6.0f};
    transform.parent = {1};
    AddComponent(scene, {2}, transform);

    CameraComponent camera;
    camera.yfov = 0.9f;
    camera.culling_mask = 0x00ff00ffU;
    AddComponent(scene, {2}, camera);

    parcel::scene::MeshRendererComponent renderer;
    renderer.mesh = Guid('a');
    renderer.materials = {Guid('b'), Guid('c')};
    renderer.submesh_materials = {0, 1};
    AddComponent(scene, {2}, renderer);

    LightComponent light;
    light.cookie = Guid('6');
    light.ies = Guid('7');
    AddComponent(scene, {2}, light);

    AnimationPlayerComponent animation;
    animation.clips = {Guid('8'), Guid('9')};
    AddComponent(scene, {2}, animation);

    SkinComponent skin;
    skin.joints = {SceneEntityId{1}, SceneEntityId{2}};
    skin.inverse_bind_matrices.resize(2);
    skin.inverse_bind_matrices[0].m[12] = 1.0f;
    skin.inverse_bind_matrices[1].m[13] = 2.0f;
    AddComponent(scene, {2}, skin);

    MorphComponent morph;
    morph.weights = {0.25f, 0.75f};
    AddComponent(scene, {2}, morph);
    return scene;
}

class MockSceneLoader final : public parcel::scene::ISceneLoader {
  public:
    parcel::core::Result<parcel::scene::SceneInstance> Instantiate(const SceneData& scene,
                                                                   void* registry) override {
        parcel::scene::SceneInstance instance;
        instance.registry = registry;
        for (const SceneEntityRecord& entity : scene.entities) {
            if (!entity.parent.IsRoot() && scene.FindEntity(entity.parent) == nullptr) {
                return parcel::core::MakeError(parcel::core::ErrorCode::Corrupt,
                                               "unresolved parent entity");
            }
            instance.entities.push_back(entity.id);
        }
        return instance;
    }

    parcel::core::Result<void> Destroy(parcel::scene::SceneInstance& instance) override {
        for (const SceneEntityId entity : instance.entities) {
            if (entity.IsRoot()) {
                return parcel::core::MakeError(parcel::core::ErrorCode::Corrupt,
                                               "invalid instance entity");
            }
        }
        instance.entities.clear();
        return {};
    }
};

} // namespace

TEST_CASE("standard component codecs round-trip") {
    CheckComponentRoundTrip(parcel::scene::TransformComponent{});
    CheckComponentRoundTrip(CameraComponent{});
    CheckComponentRoundTrip(parcel::scene::LightComponent{});
    CheckComponentRoundTrip(parcel::scene::MeshRendererComponent{});
    AnimationPlayerComponent animation_player;
    animation_player.clips = {Guid('8'), Guid('9')};
    CheckComponentRoundTrip(animation_player);
    SkinComponent skin;
    skin.joints = {SceneEntityId{1}, SceneEntityId{2}};
    skin.inverse_bind_matrices.resize(2);
    skin.inverse_bind_matrices[0].m[12] = 1.0f;
    CheckComponentRoundTrip(skin);
    MorphComponent morph;
    morph.weights = {0.25f, 0.75f};
    CheckComponentRoundTrip(morph);
    CheckComponentRoundTrip(parcel::scene::DecalComponent{});
    CheckComponentRoundTrip(parcel::scene::ReflectionProbeComponent{});
    CheckComponentRoundTrip(parcel::scene::LocalFogVolumeComponent{});
    CheckComponentRoundTrip(parcel::scene::AudioSourceComponent{});
    parcel::scene::AudioSourceMixComponent source_mix;
    source_mix.bus = parcel::scene::AudioBus::Music;
    source_mix.priority = 7;
    source_mix.occlusion_enabled = true;
    source_mix.occlusion_gain = 0.35f;
    source_mix.occlusion_low_pass_gain = 0.6f;
    source_mix.occlusion_low_pass_gain_high_frequency = 0.2f;
    CheckComponentRoundTrip(source_mix);
    parcel::scene::AudioReverbZoneComponent reverb_zone;
    reverb_zone.half_extents = {2.0f, 3.0f, 4.0f};
    reverb_zone.preset = parcel::scene::AudioReverbPreset::Room;
    reverb_zone.priority = 3;
    reverb_zone.transition_seconds = 0.25f;
    reverb_zone.enabled = false;
    CheckComponentRoundTrip(reverb_zone);
    CheckComponentRoundTrip(parcel::scene::AudioListenerComponent{});
    CHECK(StandardComponentRegistry().Find(parcel::scene::kAudioSourceMixComponentType) != nullptr);
    CHECK(StandardComponentRegistry().Find(parcel::scene::kAudioReverbZoneComponentType) !=
          nullptr);
    CheckComponentRoundTrip(parcel::scene::RigidBodyComponent{});
    CheckComponentRoundTrip(ColliderComponent{});
    parcel::scene::ScriptComponent script;
    script.class_name = "Player";
    script.properties.properties.push_back(
        {"speed", parcel::scene::PropertyType::Float, false, 0, 3.5f});
    CheckComponentRoundTrip(script);
}

TEST_CASE("morph component codec rejects excessive or non-finite weights") {
    MorphComponent too_many;
    too_many.weights.assign(parcel::scene::kMaxMorphWeights + 1, 0.0f);
    CHECK_FALSE(ComponentCodec<MorphComponent>::Encode(too_many));

    MorphComponent not_finite;
    not_finite.weights = {std::numeric_limits<float>::quiet_NaN()};
    CHECK_FALSE(ComponentCodec<MorphComponent>::Encode(not_finite));
}

TEST_CASE("component codec rejects truncated and trailing payloads") {
    const auto encoded = ComponentCodec<CameraComponent>::Encode(CameraComponent{});
    REQUIRE(encoded);
    std::vector<std::byte> payload = encoded.Value();
    payload.push_back(std::byte{0});
    CHECK_FALSE(ComponentCodec<CameraComponent>::Decode(payload));
    payload.pop_back();
    payload.pop_back();
    CHECK_FALSE(ComponentCodec<CameraComponent>::Decode(payload));
}

TEST_CASE("scene codec round-trips entities and components") {
    const SceneData scene = MakeScene();
    const auto encoded = parcel::scene::EncodeScene(scene, StandardComponentRegistry());
    REQUIRE(encoded);
    const auto decoded = parcel::scene::DecodeScene(encoded.Value(), StandardComponentRegistry());
    REQUIRE(decoded);
    CHECK(decoded.Value().entities.size() == 2);
    CHECK(decoded.Value().entities[1].parent == SceneEntityId{1});
    CHECK(decoded.Value().pipeline_settings == Guid('1'));
    REQUIRE(decoded.Value().FindComponent(parcel::scene::kCameraComponentType) != nullptr);
    const auto& chunk = *decoded.Value().FindComponent(parcel::scene::kCameraComponentType);
    REQUIRE(chunk.payloads.size() == 1);
    const auto camera = ComponentCodec<CameraComponent>::Decode(chunk.payloads[0]);
    REQUIRE(camera);
    CHECK(camera.Value().yfov == 0.9f);
}

TEST_CASE("scene validation rejects unknown type version and cycles") {
    SceneData scene = MakeScene();
    scene.components[0].type = 999;
    CHECK_FALSE(parcel::scene::EncodeScene(scene, StandardComponentRegistry()));

    scene = MakeScene();
    scene.components[0].schema_version = 99;
    CHECK_FALSE(parcel::scene::EncodeScene(scene, StandardComponentRegistry()));

    scene = MakeScene();
    scene.entities[0].parent = {2};
    CHECK_FALSE(parcel::scene::EncodeScene(scene, StandardComponentRegistry()));

    scene = MakeScene();
    ComponentChunk& skin_chunk = *scene.FindComponent(parcel::scene::kSkinComponentType);
    SkinComponent orphan_skin;
    orphan_skin.joints = {SceneEntityId{99}};
    orphan_skin.inverse_bind_matrices.resize(1);
    skin_chunk.payloads[0] = ComponentCodec<SkinComponent>::Encode(orphan_skin).Value();
    CHECK_FALSE(parcel::scene::EncodeScene(scene, StandardComponentRegistry()));

    scene = MakeScene();
    TransformComponent orphan_transform;
    orphan_transform.parent = {99};
    ComponentChunk& transform_chunk = scene.components[0];
    transform_chunk.payloads[0] =
        ComponentCodec<TransformComponent>::Encode(orphan_transform).Value();
    CHECK_FALSE(parcel::scene::EncodeScene(scene, StandardComponentRegistry()));
}

TEST_CASE("scene references include assets and settings") {
    SceneData scene = MakeScene();
    std::vector<AssetGuid> references;
    const auto result =
        parcel::scene::CollectSceneReferences(scene, StandardComponentRegistry(), references);
    REQUIRE(result);
    CHECK(std::find(references.begin(), references.end(), Guid('a')) != references.end());
    CHECK(std::find(references.begin(), references.end(), Guid('b')) != references.end());
    CHECK(std::find(references.begin(), references.end(), Guid('4')) != references.end());
    CHECK(std::find(references.begin(), references.end(), Guid('6')) != references.end());
    CHECK(std::find(references.begin(), references.end(), Guid('7')) != references.end());
    CHECK(std::find(references.begin(), references.end(), Guid('8')) != references.end());
    CHECK(std::find(references.begin(), references.end(), Guid('9')) != references.end());
}

TEST_CASE("scene loader contract creates and destroys a mock instance") {
    const SceneData scene = MakeScene();
    MockSceneLoader loader;
    auto instance = loader.Instantiate(scene, nullptr);
    REQUIRE(instance);
    CHECK(instance.Value().entities.size() == 2);
    CHECK(loader.Destroy(instance.Value()));
    CHECK(instance.Value().entities.empty());
}
