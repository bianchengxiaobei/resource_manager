#include <catch_amalgamated.hpp>

#include "parcel/assets/json.hpp"
#include "parcel/assets/sidecar.hpp"
#include "parcel/import/ies_profile.hpp"
#include "parcel/import/meta.hpp"
#include "parcel/import/sidecar_merge.hpp"
#include "parcel/scene/component_codec.hpp"
#include "parcel/scene/scene_data.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using parcel::assets::ParseJson;
using parcel::assets::ParseSceneSidecar;
using parcel::assets::SidecarProjection;

namespace {

constexpr std::string_view kSidecar = R"json(
{
  "version": 1,
  "source": "level01.fbx",
  "coordinateSystem": "rh-y-up-ccw",
  "settings": {
    "pipeline": "settings/pipeline.parcel",
    "frame": "settings/frame.parcel",
    "postProcess": "settings/post.parcel",
    "shadows": "settings/shadows.parcel",
    "volumetricFog": "settings/fog.parcel"
  },
  "lights": [{"name": "Key", "type": "directional"}],
  "decals": [{"name": "Decal"}],
  "probes": [{"name": "Probe"}],
  "localFogVolumes": [{"name": "Fog"}],
  "cameras": [{
    "name": "Main",
    "projection": "orthographic",
    "yfov": 0.8,
    "aspect": 1.5,
    "nearClip": 0.2,
    "farClip": 500,
    "xmag": 3,
    "ymag": 4,
    "clearColor": [0.1, 0.2, 0.3, 1.0],
    "cullingMask": 255,
    "priority": -1
  }]
}
)json";

} // namespace

TEST_CASE("scene sidecar parses settings cameras and override arrays") {
    const auto sidecar = ParseSceneSidecar(kSidecar);
    REQUIRE(sidecar);
    CHECK(sidecar.Value().version == 1);
    CHECK(sidecar.Value().source == "level01.fbx");
    CHECK(sidecar.Value().coordinate_system == "rh-y-up-ccw");
    CHECK(sidecar.Value().settings.pipeline == "settings/pipeline.parcel");
    CHECK(sidecar.Value().settings.post_process == "settings/post.parcel");
    CHECK(sidecar.Value().lights.size() == 1);
    CHECK(sidecar.Value().decals.size() == 1);
    CHECK(sidecar.Value().reflection_probes.size() == 1);
    CHECK(sidecar.Value().local_fog_volumes.size() == 1);
    REQUIRE(sidecar.Value().cameras.size() == 1);
    const auto& camera = sidecar.Value().cameras[0];
    CHECK(camera.name == "Main");
    CHECK(camera.projection == SidecarProjection::Orthographic);
    CHECK(camera.yfov == 0.8f);
    CHECK(camera.clear_color == parcel::Vec4{0.1f, 0.2f, 0.3f, 1.0f});
    CHECK(camera.culling_mask == 255U);
    CHECK(camera.priority == -1);
}

TEST_CASE("scene sidecar rejects invalid schemas") {
    CHECK_FALSE(ParseSceneSidecar("{}"));
    CHECK_FALSE(ParseSceneSidecar(R"({"version": 2})"));
    CHECK_FALSE(ParseSceneSidecar(R"({"version": 1, "settings": []})"));
    CHECK_FALSE(ParseSceneSidecar(R"({"version": 1, "cameras": [{"projection": "bad"}]})"));
    CHECK_FALSE(ParseSceneSidecar(R"({"version": 1, "cameras": [{"yfov": "bad"}]})"));
    CHECK_FALSE(ParseSceneSidecar(R"({"version": 1, "lights": {}})"));
    CHECK_FALSE(ParseSceneSidecar(R"({"version": 1, "settings": {"pipeline": 1}})"));
}

TEST_CASE("json parser handles escapes and rejects malformed input") {
    const auto value = ParseJson(R"({"text": "a\n\u00e9\ud83d\ude00", "enabled": true})");
    REQUIRE(value);
    const auto* text = value.Value().Find("text");
    REQUIRE(text != nullptr);
    CHECK(text->IsString());
    CHECK(text->string_value == "a\n\xc3\xa9\xf0\x9f\x98\x80");

    CHECK_FALSE(ParseJson(R"({"a": 1, "a": 2})"));
    CHECK_FALSE(ParseJson(R"([1,])"));
    CHECK_FALSE(ParseJson(R"({"a": 01})"));
    CHECK_FALSE(ParseJson(R"("unterminated)"));
}

TEST_CASE("scene sidecar loads from disk") {
    parcel::test::TempDir directory("sidecar");
    const auto path = directory.Path() / "level.parcel.json";
    {
        std::ofstream file(path, std::ios::binary);
        REQUIRE(file);
        file.write(kSidecar.data(), static_cast<std::streamsize>(kSidecar.size()));
    }
    const auto sidecar = parcel::assets::LoadSceneSidecarFile(parcel::test::ToUtf8(path));
    REQUIRE(sidecar);
    CHECK(sidecar.Value().source == "level01.fbx");
}

namespace {

void WriteSidecarFile(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(file);
}

std::string SidecarTexture(const char* name) { return std::string("assets/textures/") + name; }

} // namespace

TEST_CASE("scene sidecar merge maps decals probes and local fog") {
    parcel::test::TempDir directory("sidecar_merge");
    const std::filesystem::path project = directory.Path() / "project";
    const std::filesystem::path assets = project / "assets";
    WriteSidecarFile(assets / "textures" / "decal.png", "decal");
    WriteSidecarFile(assets / "textures" / "normal.png", "normal");
    WriteSidecarFile(assets / "textures" / "mask.png", "mask");
    WriteSidecarFile(assets / "textures" / "probe.png", "probe");
    WriteSidecarFile(assets / "textures" / "fog.png", "fog");

    constexpr std::string_view kText = R"json({
      "version": 1,
      "decals": [{
        "name": "Floor",
        "texture": "textures/decal.png",
        "normalTexture": "textures/normal.png",
        "maskTexture": "textures/mask.png",
        "position": [1.0, 2.0, 3.0],
        "rotation": [0.0, 90.0, 0.0],
        "size": [-2.0, 0.0, 4.0],
        "baseColor": [0.8, 0.7, 0.6, 0.5],
        "opacity": 1.5,
        "angleFade": 200.0,
        "normalBlend": -1.0,
        "metallic": 2.0,
        "ao": -0.2,
        "roughness": 1.2,
        "layerMask": 255,
        "channelMask": 63,
        "enabled": false
      }],
      "probes": [{
        "name": "Room",
        "texture": "textures/probe.png",
        "position": [4.0, 5.0, 6.0],
        "rotation": [0.0, 45.0, 0.0],
        "boxExtents": [-2.0, 3.0, 4.0],
        "blendDistance": -1.0,
        "intensity": -2.0,
        "enabled": false
      }],
      "localFogVolumes": [{
        "name": "Mist",
        "shape": "ellipsoid",
        "position": [7.0, 8.0, 9.0],
        "rotation": [10.0, 20.0, 30.0],
        "size": [4.0, 2.0, 6.0],
        "albedo": [0.8, 0.7, 0.6],
        "meanFreePath": 0.0,
        "anisotropy": 5.0,
        "positiveFade": [-1.0, 0.75, 1.0],
        "negativeFade": [0.25, -0.5, 0.75],
        "invertFade": true,
        "falloffMode": "exponential",
        "distanceFadeStart": 10.0,
        "distanceFadeEnd": 5.0,
        "maskTexture": "textures/fog.png",
        "maskPlane": "yz",
        "maskTiling": [2.0, 3.0, 1.0],
        "maskScroll": [0.1, 0.2, 0.0],
        "maskIntensity": 2.0,
        "maskInvert": true
      }]
    })json";
    const auto sidecar = ParseSceneSidecar(kText);
    REQUIRE(sidecar);

    parcel::scene::SceneData scene;
    std::uint64_t next_entity_id = 1;
    std::vector<std::string> warnings;
    parcel::import::SidecarMergeOptions options;
    options.project_root = parcel::test::ToUtf8(project);
    options.source_path = parcel::test::ToUtf8(assets / "level.fbx");
    REQUIRE(parcel::import::MergeSceneSidecar(scene, sidecar.Value(), options, next_entity_id,
                                              warnings));
    CHECK(warnings.empty());
    CHECK(scene.entities.size() == 3);
    CHECK(next_entity_id == 4);
    REQUIRE(scene.entities[0].name == "Floor");
    REQUIRE(scene.entities[1].name == "Room");
    REQUIRE(scene.entities[2].name == "Mist");

    const auto* decals = scene.FindComponent(parcel::scene::kDecalComponentType);
    REQUIRE(decals != nullptr);
    REQUIRE(decals->owners.size() == 1);
    const auto decal =
        parcel::scene::ComponentCodec<parcel::scene::DecalComponent>::Decode(decals->payloads[0]);
    REQUIRE(decal);
    CHECK(decal.Value().position == parcel::Vec3{1.0f, 2.0f, 3.0f});
    CHECK(decal.Value().rotation == parcel::Vec3{0.0f, 90.0f, 0.0f});
    CHECK(decal.Value().size == parcel::Vec3{2.0f, 1.0e-3f, 4.0f});
    CHECK(decal.Value().base_color == parcel::Vec4{0.8f, 0.7f, 0.6f, 0.5f});
    CHECK(decal.Value().opacity == Catch::Approx(1.0f));
    CHECK(decal.Value().angle_fade == Catch::Approx(180.0f));
    CHECK(decal.Value().normal_blend == Catch::Approx(0.0f));
    CHECK(decal.Value().metallic == Catch::Approx(1.0f));
    CHECK(decal.Value().ambient_occlusion == Catch::Approx(0.0f));
    CHECK(decal.Value().roughness == Catch::Approx(1.0f));
    CHECK(decal.Value().layer_mask == 0x7fU);
    CHECK(decal.Value().channel_mask == 0x1fU);
    CHECK_FALSE(decal.Value().enabled);
    CHECK(decal.Value().albedo ==
          parcel::import::DefaultTextureAssetGuid(SidecarTexture("decal.png")));
    CHECK(decal.Value().normal ==
          parcel::import::DefaultTextureAssetGuid(SidecarTexture("normal.png")));
    CHECK(decal.Value().mask ==
          parcel::import::DefaultTextureAssetGuid(SidecarTexture("mask.png")));

    const auto* probes = scene.FindComponent(parcel::scene::kReflectionProbeComponentType);
    REQUIRE(probes != nullptr);
    const auto probe =
        parcel::scene::ComponentCodec<parcel::scene::ReflectionProbeComponent>::Decode(
            probes->payloads[0]);
    REQUIRE(probe);
    CHECK(probe.Value().image ==
          parcel::import::DefaultTextureAssetGuid(SidecarTexture("probe.png")));
    CHECK(probe.Value().position == parcel::Vec3{4.0f, 5.0f, 6.0f});
    CHECK(probe.Value().box_extents == parcel::Vec3{2.0f, 3.0f, 4.0f});
    CHECK(probe.Value().blend_distance == Catch::Approx(0.0f));
    CHECK(probe.Value().intensity == Catch::Approx(0.0f));
    CHECK_FALSE(probe.Value().enabled);

    const auto* fogs = scene.FindComponent(parcel::scene::kLocalFogVolumeComponentType);
    REQUIRE(fogs != nullptr);
    const auto fog = parcel::scene::ComponentCodec<parcel::scene::LocalFogVolumeComponent>::Decode(
        fogs->payloads[0]);
    REQUIRE(fog);
    CHECK(fog.Value().shape == parcel::scene::LocalFogShape::Sphere);
    CHECK(fog.Value().falloff == parcel::scene::LocalFogFalloff::Exponential);
    CHECK(fog.Value().mask_plane == parcel::scene::LocalFogMaskPlane::YZ);
    CHECK(fog.Value().center == parcel::Vec3{7.0f, 8.0f, 9.0f});
    CHECK(fog.Value().mean_free_path == Catch::Approx(0.05f));
    CHECK(fog.Value().anisotropy == Catch::Approx(0.99f));
    CHECK(fog.Value().positive_fade == parcel::Vec3{0.0f, 0.75f, 1.0f});
    CHECK(fog.Value().negative_fade == parcel::Vec3{0.25f, 0.0f, 0.75f});
    CHECK(fog.Value().distance_fade_start == Catch::Approx(10.0f));
    CHECK(fog.Value().distance_fade_end > fog.Value().distance_fade_start);
    CHECK(fog.Value().mask == parcel::import::DefaultTextureAssetGuid(SidecarTexture("fog.png")));
    CHECK(fog.Value().mask_intensity == Catch::Approx(1.0f));
    CHECK(fog.Value().invert_fade);
    CHECK(fog.Value().mask_invert);

    REQUIRE(parcel::scene::ValidateScene(scene, parcel::scene::StandardComponentRegistry()));
}

TEST_CASE("scene sidecar merge warns and skips invalid component data") {
    parcel::test::TempDir directory("sidecar_merge_invalid");
    const std::filesystem::path project = directory.Path() / "project";
    const std::filesystem::path assets = project / "assets";

    const auto sidecar = ParseSceneSidecar(R"json({
      "version": 1,
      "decals": [{"name": "Bad", "opacity": "high", "size": [1, 2], "texture": 7}],
      "probes": [{"name": "NoTexture"}],
      "localFogVolumes": [{"name": "BadFog", "shape": "cone", "falloffMode": "cubic",
                            "maskPlane": "xz", "maskTexture": "missing.png"}]
    })json");
    REQUIRE(sidecar);

    parcel::scene::SceneData scene;
    std::uint64_t next_entity_id = 1;
    std::vector<std::string> warnings;
    parcel::import::SidecarMergeOptions options;
    options.project_root = parcel::test::ToUtf8(project);
    options.source_path = parcel::test::ToUtf8(assets / "level.fbx");
    REQUIRE(parcel::import::MergeSceneSidecar(scene, sidecar.Value(), options, next_entity_id,
                                              warnings));
    CHECK(scene.entities.size() == 2);
    CHECK(scene.entities[0].name == "Bad");
    CHECK(scene.entities[1].name == "BadFog");
    CHECK(warnings.size() >= 6);
    bool saw_probe_warning = false;
    bool saw_texture_warning = false;
    for (const std::string& warning : warnings) {
        saw_probe_warning =
            saw_probe_warning || warning.find("requires a valid texture") != std::string::npos;
        saw_texture_warning =
            saw_texture_warning || warning.find("missing texture") != std::string::npos;
    }
    CHECK(saw_probe_warning);
    CHECK(saw_texture_warning);

    const auto* decals = scene.FindComponent(parcel::scene::kDecalComponentType);
    REQUIRE(decals != nullptr);
    const auto decal =
        parcel::scene::ComponentCodec<parcel::scene::DecalComponent>::Decode(decals->payloads[0]);
    REQUIRE(decal);
    CHECK(decal.Value().opacity == Catch::Approx(1.0f));
    CHECK(decal.Value().size == parcel::Vec3{1.0f, 1.0f, 1.0f});
    CHECK(decal.Value().albedo.IsZero());

    const auto* fogs = scene.FindComponent(parcel::scene::kLocalFogVolumeComponentType);
    REQUIRE(fogs != nullptr);
    const auto fog = parcel::scene::ComponentCodec<parcel::scene::LocalFogVolumeComponent>::Decode(
        fogs->payloads[0]);
    REQUIRE(fog);
    CHECK(fog.Value().shape == parcel::scene::LocalFogShape::Box);
    CHECK(fog.Value().falloff == parcel::scene::LocalFogFalloff::Linear);
    CHECK(fog.Value().mask_plane == parcel::scene::LocalFogMaskPlane::XZ);
    CHECK(fog.Value().mask.IsZero());
    CHECK(fog.Value().mask_intensity == Catch::Approx(0.0f));
}

TEST_CASE("scene sidecar merge canonicalizes identical texture content") {
    parcel::test::TempDir directory("sidecar_texture_hash");
    const std::filesystem::path project = directory.Path() / "project";
    const std::filesystem::path assets = project / "assets";
    WriteSidecarFile(assets / "textures" / "albedo.png", "same bytes");
    WriteSidecarFile(assets / "textures" / "alias.png", "same bytes");

    const auto sidecar = ParseSceneSidecar(R"json({
      "version": 1,
      "decals": [{
        "name": "Floor",
        "texture": "textures/albedo.png",
        "normalTexture": "textures/alias.png"
      }]
    })json");
    REQUIRE(sidecar);

    parcel::scene::SceneData scene;
    std::uint64_t next_entity_id = 1;
    std::vector<std::string> warnings;
    std::unordered_map<std::string, parcel::core::AssetGuid> aliases;
    parcel::import::SidecarMergeOptions options;
    options.project_root = parcel::test::ToUtf8(project);
    options.source_path = parcel::test::ToUtf8(assets / "level.fbx");
    options.texture_aliases = &aliases;
    REQUIRE(parcel::import::MergeSceneSidecar(scene, sidecar.Value(), options, next_entity_id,
                                              warnings));
    CHECK(warnings.empty());
    REQUIRE(aliases.size() == 1);

    const auto* decals = scene.FindComponent(parcel::scene::kDecalComponentType);
    REQUIRE(decals != nullptr);
    const auto decal =
        parcel::scene::ComponentCodec<parcel::scene::DecalComponent>::Decode(decals->payloads[0]);
    REQUIRE(decal);
    CHECK(decal.Value().albedo == decal.Value().normal);
}

TEST_CASE("sidecar texture resolver accepts project-relative paths") {
    parcel::test::TempDir directory("sidecar_cookie_texture");
    const std::filesystem::path project = directory.Path() / "project";
    const std::filesystem::path assets = project / "assets";
    WriteSidecarFile(assets / "textures" / "cookie.png", "cookie bytes");

    parcel::import::SidecarMergeOptions options;
    options.project_root = parcel::test::ToUtf8(project);
    options.source_path = parcel::test::ToUtf8(assets / "level.fbx");
    std::unordered_map<std::string, parcel::core::AssetGuid> aliases;
    options.texture_aliases = &aliases;
    std::vector<std::string> warnings;

    const parcel::core::AssetGuid first = parcel::import::ResolveSidecarTexture(
        "textures/cookie.png", options, "light.cookie", warnings);
    CHECK_FALSE(first.IsZero());
    CHECK(warnings.empty());
    REQUIRE(aliases.size() == 1);
    const parcel::core::AssetGuid second = parcel::import::ResolveSidecarTexture(
        "textures/cookie.png", options, "light.cookie", warnings);
    CHECK(second == first);

    const parcel::core::AssetGuid missing = parcel::import::ResolveSidecarTexture(
        "textures/missing.png", options, "light.cookie", warnings);
    CHECK(missing.IsZero());
    REQUIRE(warnings.size() == 1);
}

TEST_CASE("sidecar IES resolver maps profile files to source assets") {
    parcel::test::TempDir directory("sidecar_ies");
    const std::filesystem::path project = directory.Path() / "project";
    const std::filesystem::path assets = project / "assets";
    WriteSidecarFile(assets / "lights" / "profile.ies", "IES profile");

    parcel::import::SidecarMergeOptions options;
    options.project_root = parcel::test::ToUtf8(project);
    options.source_path = parcel::test::ToUtf8(assets / "level.fbx");
    std::vector<std::string> warnings;

    const parcel::core::AssetGuid profile =
        parcel::import::ResolveSidecarIes("lights/profile.ies", options, "light.ies", warnings);
    CHECK(profile == parcel::import::DefaultIesAssetGuid("assets/lights/profile.ies"));
    CHECK(warnings.empty());

    const parcel::core::AssetGuid missing =
        parcel::import::ResolveSidecarIes("lights/missing.ies", options, "light.ies", warnings);
    CHECK(missing.IsZero());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("missing IES profile") != std::string::npos);
}

TEST_CASE("scene sidecar merge uses canonical texture index") {
    parcel::test::TempDir directory("sidecar_canonical_texture");
    const std::filesystem::path project = directory.Path() / "project";
    const std::filesystem::path assets = project / "assets";
    WriteSidecarFile(assets / "textures" / "decal.png", "decal bytes");

    const auto sidecar = ParseSceneSidecar(R"json({
      "version": 1,
      "decals": [{"name": "Floor", "texture": "textures/decal.png"}]
    })json");
    REQUIRE(sidecar);

    parcel::import::TextureCanonicalIndex index;
    const auto canonical = parcel::import::DefaultTextureAssetGuid("assets/canonical.png");
    index.by_path.emplace("assets/textures/decal.png",
                          parcel::import::TextureCanonicalAsset{"assets/canonical.png", canonical});

    parcel::scene::SceneData scene;
    std::uint64_t next_entity_id = 1;
    std::vector<std::string> warnings;
    parcel::import::SidecarMergeOptions options;
    options.project_root = parcel::test::ToUtf8(project);
    options.source_path = parcel::test::ToUtf8(assets / "level.fbx");
    options.canonical_textures = &index;
    REQUIRE(parcel::import::MergeSceneSidecar(scene, sidecar.Value(), options, next_entity_id,
                                              warnings));
    CHECK(warnings.empty());

    const auto* decals = scene.FindComponent(parcel::scene::kDecalComponentType);
    REQUIRE(decals != nullptr);
    const auto decal =
        parcel::scene::ComponentCodec<parcel::scene::DecalComponent>::Decode(decals->payloads[0]);
    REQUIRE(decal);
    CHECK(decal.Value().albedo == canonical);
}
