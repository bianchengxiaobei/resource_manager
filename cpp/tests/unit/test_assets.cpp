#include <catch_amalgamated.hpp>

#include "parcel/assets/animation_clip_asset.hpp"
#include "parcel/assets/asset_codec.hpp"
#include "parcel/assets/ies_profile_asset.hpp"
#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/mesh_asset.hpp"
#include "parcel/assets/raw_file_asset.hpp"
#include "parcel/assets/scene_asset.hpp"
#include "parcel/assets/serializers.hpp"
#include "parcel/assets/settings_assets.hpp"
#include "parcel/assets/shader_library_asset.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/scene/component_codec.hpp"
#include "parcel/serialize/asset_file.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace parcel;
using parcel::assets::AssetObject;
using parcel::assets::DecodeAssetBytes;
using parcel::assets::EncodeAssetBytes;
using parcel::assets::RegisterStandardAssetSerializers;
using parcel::assets::TypeRegistry;
using parcel::core::AssetGuid;

namespace {

AssetGuid Guid(char value) {
    std::string text = "00112233445566778899aabbccddee0";
    text.push_back(value);
    return AssetGuid::FromHex(text).Value();
}

TypeRegistry MakeRegistry() {
    TypeRegistry registry;
    REQUIRE(RegisterStandardAssetSerializers(registry));
    return registry;
}

template <typename T> std::shared_ptr<T> RoundTrip(const T& value, const TypeRegistry& registry) {
    const auto bytes = EncodeAssetBytes(value, registry);
    REQUIRE(bytes);
    const auto decoded = DecodeAssetBytes(bytes.Value(), registry);
    REQUIRE(decoded);
    auto result = std::static_pointer_cast<T>(decoded.Value());
    REQUIRE(result);
    return result;
}

} // namespace

TEST_CASE("texture asset round-trips blob mips") {
    const TypeRegistry registry = MakeRegistry();
    assets::TextureAsset texture;
    texture.format = assets::TextureFormat::BC7;
    texture.sRGB = true;
    assets::TextureMip mip;
    mip.width = 4;
    mip.height = 2;
    mip.row_pitch = 16;
    mip.data = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    texture.mips.push_back(mip);
    texture.mip_count = 1;

    const auto decoded = RoundTrip(texture, registry);
    CHECK(decoded->format == assets::TextureFormat::BC7);
    CHECK(decoded->sRGB);
    REQUIRE(decoded->mips.size() == 1);
    CHECK(decoded->mips[0].data == mip.data);
    CHECK(decoded->mip_count == 1);
}

TEST_CASE("mesh asset round-trips vertex and index blobs") {
    const TypeRegistry registry = MakeRegistry();
    assets::MeshAsset mesh;
    mesh.name = "triangle";
    mesh.vertices.push_back({});
    mesh.vertices.push_back({});
    mesh.vertices[0].position = {1.0f, 2.0f, 3.0f};
    mesh.indices = {0, 1, 0};
    assets::SubMesh sub_mesh;
    sub_mesh.index_offset = 0;
    sub_mesh.index_count = 3;
    sub_mesh.material_index = 1;
    sub_mesh.bounds.extents = {1.0f, 1.0f, 1.0f};
    mesh.sub_meshes.push_back(sub_mesh);
    mesh.bounds.extents = {2.0f, 2.0f, 2.0f};
    mesh.has_normals = true;
    mesh.skin_vertices.resize(2);
    mesh.skin_vertices[0].joints = {0, 1, 0, 0};
    mesh.skin_vertices[0].weights = {0.75f, 0.25f, 0.0f, 0.0f};
    mesh.skin_vertices[1].joints = {1, 0, 0, 0};
    mesh.skin_vertices[1].weights = {1.0f, 0.0f, 0.0f, 0.0f};
    assets::MorphTarget morph;
    morph.name = "Smile";
    morph.position_deltas = {{0.1f, 0.0f, 0.0f}, {0.0f, 0.2f, 0.0f}};
    morph.normal_deltas = {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}};
    morph.tangent_deltas = {{0.1f, 0.2f, 0.3f, 0.0f}, {0.4f, 0.5f, 0.6f, 0.0f}};
    morph.has_normals = true;
    morph.has_tangents = true;
    mesh.morph_targets.push_back(morph);
    mesh.morph_weights = {0.25f};

    const auto decoded = RoundTrip(mesh, registry);
    CHECK(decoded->name == "triangle");
    REQUIRE(decoded->vertices.size() == 2);
    CHECK(decoded->vertices[0].position == parcel::Vec3{1.0f, 2.0f, 3.0f});
    CHECK(decoded->indices == mesh.indices);
    REQUIRE(decoded->sub_meshes.size() == 1);
    CHECK(decoded->sub_meshes[0].material_index == 1);
    CHECK(decoded->has_normals);
    REQUIRE(decoded->skin_vertices.size() == 2);
    CHECK(decoded->skin_vertices[0].joints == mesh.skin_vertices[0].joints);
    CHECK(decoded->skin_vertices[0].weights == mesh.skin_vertices[0].weights);
    REQUIRE(decoded->morph_targets.size() == 1);
    CHECK(decoded->morph_targets[0].name == "Smile");
    CHECK(decoded->morph_targets[0].position_deltas == morph.position_deltas);
    CHECK(decoded->morph_targets[0].normal_deltas == morph.normal_deltas);
    CHECK(decoded->morph_targets[0].tangent_deltas == morph.tangent_deltas);
    CHECK(decoded->morph_targets[0].has_normals);
    CHECK(decoded->morph_targets[0].has_tangents);
    CHECK(decoded->morph_weights == mesh.morph_weights);

    assets::MeshAsset invalid = mesh;
    invalid.skin_vertices.pop_back();
    const auto invalid_bytes = EncodeAssetBytes(invalid, registry);
    CHECK_FALSE(invalid_bytes);
    CHECK(invalid_bytes.GetError().code == core::ErrorCode::InvalidArgument);

    invalid = mesh;
    invalid.morph_targets[0].position_deltas.pop_back();
    const auto invalid_morph_bytes = EncodeAssetBytes(invalid, registry);
    CHECK_FALSE(invalid_morph_bytes);
    CHECK(invalid_morph_bytes.GetError().code == core::ErrorCode::InvalidArgument);

    invalid = mesh;
    invalid.morph_weights.push_back(0.5f);
    const auto invalid_weight_bytes = EncodeAssetBytes(invalid, registry);
    CHECK_FALSE(invalid_weight_bytes);
    CHECK(invalid_weight_bytes.GetError().code == core::ErrorCode::InvalidArgument);
}

TEST_CASE("material asset round-trips texture references") {
    const TypeRegistry registry = MakeRegistry();
    assets::MaterialAsset material;
    material.name = "paint";
    material.type = assets::MaterialType::Unlit;
    material.alpha_mode = assets::AlphaMode::Blend;
    material.base_color_texture.texture = Guid('a');
    material.base_color_texture.uv_set = 1;
    material.base_color_texture.sampler.wrap_s = assets::TextureWrap::Repeat;
    material.base_color_texture.sampler.wrap_t = assets::TextureWrap::Mirror;
    material.base_color_texture.sampler.min_filter = assets::TextureFilter::Nearest;
    material.base_color_texture.sampler.mag_filter = assets::TextureFilter::Nearest;
    material.base_color_texture.sampler.mip_filter = assets::TextureMipFilter::Nearest;
    material.normal_texture.texture = Guid('b');
    material.clear_coat = 0.5f;

    const auto desc = assets::EncodeAsset(material, registry);
    REQUIRE(desc);
    CHECK(desc.Value().external_refs.size() == 2);

    const auto decoded = RoundTrip(material, registry);
    CHECK(decoded->name == "paint");
    CHECK(decoded->type == assets::MaterialType::Unlit);
    CHECK(decoded->alpha_mode == assets::AlphaMode::Blend);
    CHECK(decoded->base_color_texture.texture == Guid('a'));
    CHECK(decoded->base_color_texture.uv_set == 1);
    CHECK(decoded->base_color_texture.sampler.wrap_s == assets::TextureWrap::Repeat);
    CHECK(decoded->base_color_texture.sampler.wrap_t == assets::TextureWrap::Mirror);
    CHECK(decoded->base_color_texture.sampler.min_filter == assets::TextureFilter::Nearest);
    CHECK(decoded->base_color_texture.sampler.mag_filter == assets::TextureFilter::Nearest);
    CHECK(decoded->base_color_texture.sampler.mip_filter == assets::TextureMipFilter::Nearest);
    CHECK(decoded->normal_texture.sampler == assets::TextureSampler{});
    CHECK(decoded->clear_coat == 0.5f);
}

TEST_CASE("shader library asset round-trips pass binaries") {
    const TypeRegistry registry = MakeRegistry();
    assets::ShaderLibraryAsset shader;
    shader.name = "hdrp";
    assets::ShaderBackendLibrary backend;
    backend.backend = assets::ShaderBackend::Spirv;
    backend.passes.push_back({"forward", assets::ShaderStage::Vertex, 0, {std::byte{1}}});
    backend.variants = {"default"};
    shader.backends.push_back(backend);
    shader.pipeline_key = "hdrp";

    const auto decoded = RoundTrip(shader, registry);
    CHECK(decoded->name == "hdrp");
    REQUIRE(decoded->backends.size() == 1);
    REQUIRE(decoded->backends[0].passes.size() == 1);
    CHECK(decoded->backends[0].passes[0].pass == "forward");
    CHECK(decoded->backends[0].passes[0].data == std::vector<std::byte>{std::byte{1}});
}

TEST_CASE("raw and text assets round-trip") {
    const TypeRegistry registry = MakeRegistry();
    assets::RawFileAsset raw;
    raw.name = "readme";
    raw.data = {std::byte{9}, std::byte{8}};
    const auto decoded_raw = RoundTrip(raw, registry);
    CHECK(decoded_raw->data == raw.data);

    assets::TextAsset text;
    text.name = "config";
    text.text = "hello";
    const auto decoded_text = RoundTrip(text, registry);
    CHECK(decoded_text->text == "hello");
}

TEST_CASE("settings assets round-trip versioned fields") {
    const TypeRegistry registry = MakeRegistry();

    assets::PipelineSettingsAsset pipeline;
    pipeline.settings.support_shadows = false;
    pipeline.settings.support_ssr = true;
    const auto decoded_pipeline = RoundTrip(pipeline, registry);
    CHECK_FALSE(decoded_pipeline->settings.support_shadows);
    CHECK(decoded_pipeline->settings.support_ssr);

    assets::FrameSettingsAsset frame;
    frame.settings.SetEnabled(assets::FrameSettingsField::SSR);
    frame.settings.lod_bias = 1.5f;
    const auto decoded_frame = RoundTrip(frame, registry);
    CHECK(decoded_frame->settings.IsEnabled(assets::FrameSettingsField::SSR));
    CHECK(decoded_frame->settings.lod_bias == 1.5f);

    assets::PostProcessProfileAsset post;
    post.profile.name = "night";
    post.profile.settings.bloom.enabled = true;
    post.profile.settings.bloom.intensity = 2.0f;
    const auto decoded_post = RoundTrip(post, registry);
    CHECK(decoded_post->profile.name == "night");
    CHECK(decoded_post->profile.settings.bloom.enabled);
    CHECK(decoded_post->profile.settings.bloom.intensity == 2.0f);

    assets::ShadowSettingsAsset shadow;
    shadow.settings.area_filter = assets::ShadowFilter::Pcss;
    shadow.settings.contact_shadows.enabled = true;
    const auto decoded_shadow = RoundTrip(shadow, registry);
    CHECK(decoded_shadow->settings.area_filter == assets::ShadowFilter::Pcss);
    CHECK(decoded_shadow->settings.contact_shadows.enabled);

    assets::VolumetricFogProfileAsset fog;
    fog.settings.enabled = true;
    fog.settings.step_count = 32;
    const auto decoded_fog = RoundTrip(fog, registry);
    CHECK(decoded_fog->settings.enabled);
    CHECK(decoded_fog->settings.step_count == 32);
}

TEST_CASE("scene asset round-trips scene data") {
    const TypeRegistry registry = MakeRegistry();
    assets::SceneAsset scene;
    scene.name = "level";
    scene.scene.pipeline_settings = Guid('1');
    scene::SceneEntityRecord entity;
    entity.id = {1};
    entity.name = "Camera";
    scene.scene.entities.push_back(entity);
    assets::TypeId type = scene.Type();
    CHECK(type == assets::kSceneAssetType);

    const auto decoded = RoundTrip(scene, registry);
    CHECK(decoded->name == "level");
    REQUIRE(decoded->scene.entities.size() == 1);
    CHECK(decoded->scene.entities[0].name == "Camera");
    CHECK(decoded->scene.pipeline_settings == Guid('1'));
}

TEST_CASE("IES profile asset round-trips angle tables and rejects invalid data") {
    const TypeRegistry registry = MakeRegistry();
    assets::IesProfileAsset profile;
    profile.name = "studio.ies";
    profile.vertical_angles = {0.0f, 45.0f, 90.0f};
    profile.horizontal_angles = {0.0f, 90.0f, 180.0f, 270.0f};
    profile.values.assign(12, 1.0f);
    profile.values[4] = 0.5f;
    CHECK(profile.Type() == assets::kIesProfileAssetType);

    const auto decoded = RoundTrip(profile, registry);
    CHECK(decoded->name == "studio.ies");
    CHECK(decoded->vertical_angles == profile.vertical_angles);
    CHECK(decoded->horizontal_angles == profile.horizontal_angles);
    CHECK(decoded->values == profile.values);
    REQUIRE(parcel::assets::ValidateIesProfile(*decoded));

    assets::IesProfileAsset invalid = profile;
    invalid.values.pop_back();
    const auto bytes = EncodeAssetBytes(invalid, registry);
    CHECK_FALSE(bytes);
    CHECK(bytes.GetError().code == core::ErrorCode::InvalidArgument);
}

TEST_CASE("animation clip asset round-trips channels and rejects invalid data") {
    const TypeRegistry registry = MakeRegistry();
    assets::AnimationClipAsset clip;
    clip.name = "Walk";
    clip.duration = 2.0f;
    assets::AnimationChannel rotation;
    rotation.target = {7};
    rotation.path = assets::AnimationPath::Rotation;
    rotation.interpolation = assets::AnimationInterpolation::Linear;
    rotation.times = {0.0f, 1.0f, 2.0f};
    rotation.values = {
        {0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.5f, 0.0f, 0.8660254f}, {0.0f, 1.0f, 0.0f, 0.0f}};
    clip.channels.push_back(rotation);
    assets::AnimationChannel scale;
    scale.target = {7};
    scale.path = assets::AnimationPath::Scale;
    scale.interpolation = assets::AnimationInterpolation::CubicSpline;
    scale.times = {0.0f, 2.0f};
    scale.values = {{1.0f, 1.0f, 1.0f, 0.0f}, {2.0f, 2.0f, 2.0f, 0.0f}};
    scale.in_tangents = {{0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}};
    scale.out_tangents = {{0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}};
    clip.channels.push_back(scale);
    CHECK(clip.Type() == assets::kAnimationClipAssetType);
    REQUIRE(parcel::assets::ValidateAnimationClip(clip));

    const auto decoded = RoundTrip(clip, registry);
    CHECK(decoded->name == "Walk");
    CHECK(decoded->duration == Catch::Approx(2.0f));
    REQUIRE(decoded->channels.size() == 2);
    CHECK(decoded->channels[0].target == parcel::scene::SceneEntityId{7});
    CHECK(decoded->channels[0].path == assets::AnimationPath::Rotation);
    CHECK(decoded->channels[0].times == rotation.times);
    CHECK(decoded->channels[1].in_tangents.size() == 2);
    CHECK(decoded->channels[1].out_tangents.size() == 2);

    assets::AnimationClipAsset invalid = clip;
    invalid.channels[0].target = {};
    CHECK_FALSE(assets::ValidateAnimationClip(invalid));
    invalid = clip;
    invalid.channels[0].times[2] = 3.0f;
    CHECK_FALSE(assets::ValidateAnimationClip(invalid));
    invalid = clip;
    invalid.channels[1].in_tangents.pop_back();
    CHECK_FALSE(assets::ValidateAnimationClip(invalid));
    const auto bytes = EncodeAssetBytes(invalid, registry);
    CHECK_FALSE(bytes);
}

TEST_CASE("unknown asset type and serializer version are rejected") {
    const TypeRegistry registry = MakeRegistry();
    assets::TextAsset text;
    text.text = "x";
    const auto bytes = EncodeAssetBytes(text, registry);
    REQUIRE(bytes);

    TypeRegistry empty;
    const auto unknown = DecodeAssetBytes(bytes.Value(), empty);
    CHECK_FALSE(unknown);
    CHECK(unknown.GetError().code == core::ErrorCode::Unsupported);

    std::vector<std::byte> patched = bytes.Value();
    serialize::AssetFileReader reader(patched);
    REQUIRE(reader.ReadHeader());
    const std::uint64_t type_table = reader.Header().type_table_offset;
    const std::size_t version_offset = static_cast<std::size_t>(type_table) + 8;
    REQUIRE(version_offset + 4 <= patched.size());
    const std::uint32_t version = 99;
    for (std::size_t i = 0; i < 4; ++i) {
        patched[version_offset + i] = static_cast<std::byte>((version >> (i * 8U)) & 0xffU);
    }
    const auto version_error = DecodeAssetBytes(patched, registry, nullptr, false);
    CHECK_FALSE(version_error);
    CHECK(version_error.GetError().code == core::ErrorCode::Unsupported);
}
