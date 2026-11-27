#include <catch_amalgamated.hpp>

#include "../../src/import/texture_decode.hpp"
#include "parcel/assets/animation_clip_asset.hpp"
#include "parcel/assets/asset_codec.hpp"
#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/mesh_asset.hpp"
#include "parcel/assets/scene_asset.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/import/importers.hpp"
#include "parcel/import/meta.hpp"
#include "parcel/import/pipeline.hpp"
#include "parcel/pack/pack.hpp"
#include "parcel/scene/component_codec.hpp"
#include "parcel/scene/components.hpp"
#include "parcel/scene/scene_data.hpp"
#include "test_util.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using parcel::assets::AnimationClipAsset;
using parcel::assets::AssetObject;
using parcel::assets::MaterialAsset;
using parcel::assets::MeshAsset;
using parcel::assets::SceneAsset;
using parcel::core::AssetGuid;
using parcel::scene::AnimationPlayerComponent;
using parcel::scene::CameraComponent;
using parcel::scene::LightComponent;
using parcel::scene::MeshRendererComponent;
using parcel::scene::MorphComponent;
using parcel::scene::SkinComponent;
using parcel::test::TempDir;
using parcel::test::ToUtf8;

namespace {

std::string Utf8(const std::filesystem::path& path) { return ToUtf8(path); }

void WriteText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(file);
}

void WriteBinary(const std::filesystem::path& path, std::span<const std::byte> data) {
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    REQUIRE(file);
}

std::vector<std::byte> TgaBytes() {
    const std::array<std::uint8_t, 22> pixels = {0, 0, 2, 0, 0, 0,  0, 0, 0, 0,   0,
                                                 0, 1, 0, 1, 0, 32, 8, 0, 0, 255, 255};
    std::vector<std::byte> bytes(sizeof(pixels));
    std::memcpy(bytes.data(), pixels.data(), bytes.size());
    return bytes;
}

std::vector<std::byte> HorizontalTga() {
    const std::array<std::uint8_t, 34> pixels = {
        0, 0, 2, 0,   0,   0, 0,   0, 0,   0,   0, 0, 4,   0,   1,   0,   32,
        8, 0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255,
    };
    std::vector<std::byte> bytes(sizeof(pixels));
    std::memcpy(bytes.data(), pixels.data(), bytes.size());
    return bytes;
}

void WriteTga(const std::filesystem::path& path) {
    const std::vector<std::byte> pixels = TgaBytes();
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(reinterpret_cast<const char*>(pixels.data()),
               static_cast<std::streamsize>(pixels.size()));
    REQUIRE(file);
}

std::string Base64(std::span<const std::byte> bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const std::uint32_t first = std::to_integer<std::uint32_t>(bytes[index]);
        const std::uint32_t second =
            index + 1 < bytes.size() ? std::to_integer<std::uint32_t>(bytes[index + 1]) : 0;
        const std::uint32_t third =
            index + 2 < bytes.size() ? std::to_integer<std::uint32_t>(bytes[index + 2]) : 0;
        const std::uint32_t value = (first << 16) | (second << 8) | third;
        result.push_back(kAlphabet[(value >> 18) & 0x3f]);
        result.push_back(kAlphabet[(value >> 12) & 0x3f]);
        result.push_back(index + 1 < bytes.size() ? kAlphabet[(value >> 6) & 0x3f] : '=');
        result.push_back(index + 2 < bytes.size() ? kAlphabet[value & 0x3f] : '=');
    }
    return result;
}

void StoreU16(std::span<std::byte> data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<std::byte>(value & 0xffU);
    data[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void StoreU32(std::span<std::byte> data, std::size_t offset, std::uint32_t value) {
    data[offset] = static_cast<std::byte>(value & 0xffU);
    data[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    data[offset + 2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    data[offset + 3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

void StoreF32(std::span<std::byte> data, std::size_t offset, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    StoreU32(data, offset, bits);
}

std::vector<std::byte> MakeTriangleBuffer() {
    constexpr std::size_t kPositions = 0;
    constexpr std::size_t kNormals = 36;
    constexpr std::size_t kUvs = 72;
    constexpr std::size_t kIndices = 96;
    constexpr std::size_t kMorph = 104;
    constexpr std::size_t kTimes = 140;
    constexpr std::size_t kTranslation = 148;
    constexpr std::size_t kWeights = 172;
    constexpr std::size_t kJoints = 180;
    constexpr std::size_t kSkinWeights = 192;
    constexpr std::size_t kInverseBind = 240;
    constexpr std::size_t kSize = 368;

    std::vector<std::byte> buffer(kSize, std::byte{0});
    const std::array<std::array<float, 3>, 3> positions = {
        {{{0.0f, 0.0f, 0.0f}}, {{1.0f, 0.0f, 0.0f}}, {{0.0f, 1.0f, 0.0f}}}};
    const std::array<std::array<float, 3>, 3> morph = {
        {{{0.5f, 0.0f, 0.0f}}, {{0.0f, 0.25f, 0.0f}}, {{0.0f, 0.0f, 0.75f}}}};
    const std::array<std::array<float, 2>, 3> uvs = {
        {{{0.0f, 0.0f}}, {{1.0f, 0.0f}}, {{0.0f, 1.0f}}}};
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
        for (std::size_t component = 0; component < 3; ++component) {
            StoreF32(buffer, kPositions + vertex * 12 + component * 4,
                     positions[vertex][component]);
            StoreF32(buffer, kNormals + vertex * 12 + component * 4, component == 2 ? 1.0f : 0.0f);
            StoreF32(buffer, kMorph + vertex * 12 + component * 4, morph[vertex][component]);
        }
        StoreF32(buffer, kUvs + vertex * 8, uvs[vertex][0]);
        StoreF32(buffer, kUvs + vertex * 8 + 4, uvs[vertex][1]);
        StoreU16(buffer, kIndices + vertex * 2, static_cast<std::uint16_t>(vertex));
        buffer[kJoints + vertex * 4] = std::byte{0};
        buffer[kJoints + vertex * 4 + 1] = std::byte{1};
        buffer[kJoints + vertex * 4 + 2] = std::byte{0};
        buffer[kJoints + vertex * 4 + 3] = std::byte{0};
        StoreF32(buffer, kSkinWeights + vertex * 16, 0.75f);
        StoreF32(buffer, kSkinWeights + vertex * 16 + 4, 0.25f);
        StoreF32(buffer, kSkinWeights + vertex * 16 + 8, 0.0f);
        StoreF32(buffer, kSkinWeights + vertex * 16 + 12, 0.0f);
    }
    StoreF32(buffer, kTimes, 0.0f);
    StoreF32(buffer, kTimes + 4, 1.0f);
    StoreF32(buffer, kTranslation, 0.0f);
    StoreF32(buffer, kTranslation + 4, 0.0f);
    StoreF32(buffer, kTranslation + 8, 0.0f);
    StoreF32(buffer, kTranslation + 12, 1.0f);
    StoreF32(buffer, kTranslation + 16, 0.0f);
    StoreF32(buffer, kTranslation + 20, 0.0f);
    StoreF32(buffer, kWeights, 0.0f);
    StoreF32(buffer, kWeights + 4, 1.0f);
    for (std::size_t joint = 0; joint < 2; ++joint) {
        for (std::size_t element = 0; element < 16; ++element) {
            StoreF32(buffer, kInverseBind + joint * 64 + element * 4,
                     element % 5 == 0 ? 1.0f : 0.0f);
        }
    }
    return buffer;
}

std::string TriangleGltf(bool external_buffer) {
    const auto path = std::filesystem::path(PARCEL_GLTF_DATA_DIR) / "triangle.gltf";
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!external_buffer) {
        const std::string uri = "\"uri\": \"triangle.bin\", ";
        const auto found = text.find(uri);
        REQUIRE(found != std::string::npos);
        text.erase(found, uri.size());
    }
    return text;
}
void WriteGlb(const std::filesystem::path& path, std::string json,
              std::span<const std::byte> binary) {
    while (json.size() % 4 != 0) {
        json = std::string(json) + " ";
    }
    std::vector<std::byte> data;
    const std::uint32_t json_size = static_cast<std::uint32_t>(json.size());
    const std::uint32_t bin_padding = static_cast<std::uint32_t>((4 - binary.size() % 4) % 4);
    const std::uint32_t bin_size = static_cast<std::uint32_t>(binary.size() + bin_padding);
    const std::uint32_t total_size = 12 + 8 + json_size + 8 + bin_size;
    data.resize(total_size);
    data[0] = std::byte{'g'};
    data[1] = std::byte{'l'};
    data[2] = std::byte{'T'};
    data[3] = std::byte{'F'};
    StoreU32(data, 4, 2);
    StoreU32(data, 8, total_size);
    StoreU32(data, 12, json_size);
    StoreU32(data, 16, 0x4e4f534aU);
    std::memcpy(data.data() + 20, json.data(), json.size());
    const std::size_t bin_header = 20 + json_size;
    StoreU32(data, bin_header, bin_size);
    StoreU32(data, bin_header + 4, 0x004e4942U);
    std::memcpy(data.data() + bin_header + 8, binary.data(), binary.size());
    WriteBinary(path, data);
}

struct GltfFixture {
    TempDir directory{"gltf"};
    std::filesystem::path project = directory.Path() / "project";
    std::filesystem::path assets = project / "assets";
    std::vector<std::byte> buffer = MakeTriangleBuffer();

    GltfFixture() {
        std::filesystem::create_directories(assets / "textures");
        WriteTga(assets / "textures" / "checker.tga");
        WriteBinary(assets / "triangle.bin", buffer);
        WriteText(assets / "triangle.gltf", TriangleGltf(true));
        WriteGlb(assets / "triangle.glb", TriangleGltf(false), buffer);
    }

    void UseEmbeddedImage(bool data_uri) {
        const std::vector<std::byte> image = TgaBytes();
        const std::string old_image = "\"images\": [{\"uri\": \"textures/checker.tga\"}]";
        std::string text = TriangleGltf(true);
        const auto image_position = text.find(old_image);
        REQUIRE(image_position != std::string::npos);
        if (data_uri) {
            text.replace(image_position, old_image.size(),
                         "\"images\": [{\"uri\": \"data:image/x-tga;base64," + Base64(image) +
                             "\"}]");
        } else {
            text.replace(image_position, old_image.size(),
                         "\"images\": [{\"bufferView\": 11, \"mimeType\": \"image/x-tga\"}]");
            const std::size_t image_offset = buffer.size();
            buffer.insert(buffer.end(), image.begin(), image.end());
            const std::string old_length = "\"uri\": \"triangle.bin\", \"byteLength\": 368";
            const auto length_position = text.find(old_length);
            REQUIRE(length_position != std::string::npos);
            text.replace(length_position, old_length.size(),
                         "\"uri\": \"triangle.bin\", \"byteLength\": " +
                             std::to_string(image_offset + image.size()));
            const std::string old_view =
                "    {\"buffer\": 0, \"byteOffset\": 240, \"byteLength\": 128}\n  ],";
            const auto view_position = text.find(old_view);
            REQUIRE(view_position != std::string::npos);
            text.replace(view_position, old_view.size(),
                         "    {\"buffer\": 0, \"byteOffset\": 240, \"byteLength\": 128},\n"
                         "    {\"buffer\": 0, \"byteOffset\": " +
                             std::to_string(image_offset) +
                             ", \"byteLength\": " + std::to_string(image.size()) + "}\n  ],");
            WriteBinary(assets / "triangle.bin", buffer);
        }
        WriteText(assets / "triangle.gltf", text);
    }

    void UseParentRelativePaths() {
        std::filesystem::create_directories(assets / "scenes");
        std::filesystem::create_directories(assets / "buffers");
        std::filesystem::copy_file(assets / "triangle.bin", assets / "buffers" / "triangle.bin",
                                   std::filesystem::copy_options::overwrite_existing);

        std::string text = TriangleGltf(true);
        const std::string old_buffer = "\"uri\": \"triangle.bin\"";
        const auto buffer_position = text.find(old_buffer);
        REQUIRE(buffer_position != std::string::npos);
        text.replace(buffer_position, old_buffer.size(), "\"uri\": \"../buffers/triangle.bin\"");

        const std::string old_image = "\"images\": [{\"uri\": \"textures/checker.tga\"}]";
        const auto image_position = text.find(old_image);
        REQUIRE(image_position != std::string::npos);
        text.replace(image_position, old_image.size(),
                     "\"images\": [{\"uri\": \"../textures/checker.tga\"}]");

        WriteText(assets / "scenes" / "triangle.gltf", text);
    }

    void UseMaterialVariants() {
        std::string text = TriangleGltf(true);
        const std::string old_used = "\"extensionsUsed\": [\"KHR_lights_punctual\"],";
        const auto used_position = text.find(old_used);
        REQUIRE(used_position != std::string::npos);
        text.replace(used_position, old_used.size(),
                     "\"extensionsUsed\": [\"KHR_lights_punctual\", \"KHR_materials_variants\"],\n"
                     "  \"extensionsRequired\": [\"KHR_materials_variants\"],");

        const std::string old_extensions = "\"extensions\": {\"KHR_lights_punctual\": {\"lights\":";
        const auto extensions_position = text.find(old_extensions);
        REQUIRE(extensions_position != std::string::npos);
        text.replace(extensions_position, old_extensions.size(),
                     "\"extensions\": {\"KHR_materials_variants\": {\"variants\": "
                     "[{\"name\": \"Blue\"}, {\"name\": \"Red\"}]}, "
                     "\"KHR_lights_punctual\": {\"lights\":");

        const std::string old_materials_end = "  }],\n  \"meshes\":";
        const auto materials_position = text.find(old_materials_end);
        REQUIRE(materials_position != std::string::npos);
        text.replace(materials_position, old_materials_end.size(),
                     "  }, {\"name\": \"VariantBlue\", \"pbrMetallicRoughness\": "
                     "{\"baseColorFactor\": [0.1, 0.2, 0.9, 1.0], \"metallicFactor\": 0.0, "
                     "\"roughnessFactor\": 0.5}}],\n  \"meshes\":");

        const std::string old_primitive = "      \"material\": 0,\n      \"mode\": 4,\n";
        const auto primitive_position = text.find(old_primitive);
        REQUIRE(primitive_position != std::string::npos);
        text.replace(primitive_position, old_primitive.size(),
                     "      \"material\": 0,\n      \"mode\": 4,\n"
                     "      \"extensions\": {\"KHR_materials_variants\": {\"mappings\": "
                     "[{\"material\": 1, \"variants\": [0]}]}},\n");

        WriteText(assets / "variants.gltf", text);
    }

    void UseTextureTransform(bool shared) {
        std::string text = TriangleGltf(true);
        const std::string old_used = "\"extensionsUsed\": [\"KHR_lights_punctual\"],";
        const auto used_position = text.find(old_used);
        REQUIRE(used_position != std::string::npos);
        text.replace(used_position, old_used.size(),
                     "\"extensionsUsed\": [\"KHR_lights_punctual\", "
                     "\"KHR_texture_transform\"],");

        const std::string transform =
            "\"extensions\": {\"KHR_texture_transform\": {\"offset\": [0.25, 0.5], "
            "\"rotation\": 1.5707964, \"scale\": [2.0, 3.0]}}";
        const std::string old_base = "\"baseColorTexture\": {\"index\": 0}";
        const auto base_position = text.find(old_base);
        REQUIRE(base_position != std::string::npos);
        text.replace(base_position, old_base.size(),
                     "\"baseColorTexture\": {\"index\": 0, " + transform + "}");
        if (shared) {
            const std::string old_normal = "\"normalTexture\": {\"index\": 1}";
            const auto normal_position = text.find(old_normal);
            REQUIRE(normal_position != std::string::npos);
            text.replace(normal_position, old_normal.size(),
                         "\"normalTexture\": {\"index\": 1, " + transform + "}");
        }

        WriteText(assets / "transform.gltf", text);
    }

    void UseScenes() {
        std::string text = TriangleGltf(true);
        const std::string old_scenes = "\"scenes\": [{\"nodes\": [0]}],\n  \"scene\": 0,";
        const auto scenes_position = text.find(old_scenes);
        REQUIRE(scenes_position != std::string::npos);
        text.replace(scenes_position, old_scenes.size(),
                     "\"scenes\": [{\"name\": \"Main\", \"nodes\": [0]}, "
                     "{\"name\": \"Extra\", \"nodes\": [6]}],\n  \"scene\": 0,");

        const std::string old_nodes_end = "\n  ],\n  \"scenes\"";
        const auto nodes_position = text.find(old_nodes_end);
        REQUIRE(nodes_position != std::string::npos);
        text.replace(nodes_position, old_nodes_end.size(),
                     ",\n    {\"name\": \"ExtraNode\", \"mesh\": 0}\n  ],\n  \"scenes\"");

        WriteText(assets / "scenes.gltf", text);
    }

    void UseMeshoptCompression() {
        const std::array<float, 9> positions = {
            0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        };
        const std::array<std::uint32_t, 3> indices = {0, 1, 2};
        std::vector<unsigned char> encoded_positions(
            meshopt_encodeVertexBufferBound(3, sizeof(float) * 3));
        const std::size_t position_size =
            meshopt_encodeVertexBuffer(encoded_positions.data(), encoded_positions.size(),
                                       positions.data(), 3, sizeof(float) * 3);
        REQUIRE(position_size > 0);
        encoded_positions.resize(position_size);

        std::vector<unsigned char> encoded_indices(meshopt_encodeIndexBufferBound(3, 3));
        const std::size_t index_size = meshopt_encodeIndexBuffer(
            encoded_indices.data(), encoded_indices.size(), indices.data(), 3);
        REQUIRE(index_size > 0);
        encoded_indices.resize(index_size);

        std::vector<std::byte> compressed(position_size + index_size);
        std::memcpy(compressed.data(), encoded_positions.data(), position_size);
        std::memcpy(compressed.data() + position_size, encoded_indices.data(), index_size);
        WriteBinary(assets / "meshopt.bin", compressed);

        std::string text = TriangleGltf(true);
        const std::string old_used = "\"extensionsUsed\": [\"KHR_lights_punctual\"],";
        const auto used_position = text.find(old_used);
        REQUIRE(used_position != std::string::npos);
        text.replace(used_position, old_used.size(),
                     "\"extensionsUsed\": [\"KHR_lights_punctual\", "
                     "\"EXT_meshopt_compression\"],\n"
                     "  \"extensionsRequired\": [\"EXT_meshopt_compression\"],");

        const std::string old_buffers =
            "\"buffers\": [{\"uri\": \"triangle.bin\", \"byteLength\": 368}],";
        const auto buffers_position = text.find(old_buffers);
        REQUIRE(buffers_position != std::string::npos);
        text.replace(buffers_position, old_buffers.size(),
                     "\"buffers\": [{\"uri\": \"triangle.bin\", \"byteLength\": 368}, "
                     "{\"uri\": \"meshopt.bin\", \"byteLength\": " +
                         std::to_string(compressed.size()) + "}],");

        const std::string old_position_view =
            "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36},";
        const auto position_view_position = text.find(old_position_view);
        REQUIRE(position_view_position != std::string::npos);
        text.replace(position_view_position, old_position_view.size(),
                     "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36, "
                     "\"extensions\": {\"EXT_meshopt_compression\": {\"buffer\": 1, "
                     "\"byteOffset\": 0, \"byteLength\": " +
                         std::to_string(position_size) +
                         ", \"byteStride\": 12, \"count\": 3, \"mode\": \"ATTRIBUTES\", "
                         "\"filter\": \"NONE\"}}},");

        const std::string old_index_view =
            "    {\"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 6},";
        const auto index_view_position = text.find(old_index_view);
        REQUIRE(index_view_position != std::string::npos);
        text.replace(index_view_position, old_index_view.size(),
                     "    {\"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 6, "
                     "\"extensions\": {\"EXT_meshopt_compression\": {\"buffer\": 1, "
                     "\"byteOffset\": " +
                         std::to_string(position_size) +
                         ", \"byteLength\": " + std::to_string(index_size) +
                         ", \"byteStride\": 2, \"count\": 3, \"mode\": \"TRIANGLES\", "
                         "\"filter\": \"NONE\"}}},");

        WriteText(assets / "meshopt.gltf", text);
    }
};

parcel::import::ImportRequest Request(const GltfFixture& fixture, const std::string& file,
                                      const parcel::assets::JsonValue& settings) {
    parcel::import::ImportRequest request;
    request.project_root = Utf8(fixture.project);
    request.source_path = Utf8(fixture.assets / file);
    request.project_relative_path = "assets/" + file;
    request.source_guid = parcel::import::MakeSourceGuid(request.project_relative_path);
    request.settings = &settings;
    return request;
}

const AssetObject* FindAsset(const parcel::import::ImportResult& result, std::string_view role) {
    for (const parcel::import::CookedAsset& asset : result.assets) {
        if (asset.role == role) {
            return asset.object.get();
        }
    }
    return nullptr;
}

const parcel::import::CookedAsset* FindCooked(const parcel::import::ImportResult& result,
                                              std::string_view role) {
    for (const parcel::import::CookedAsset& asset : result.assets) {
        if (asset.role == role) {
            return &asset;
        }
    }
    return nullptr;
}

const MaterialAsset* SceneMaterial(const parcel::import::ImportResult& result,
                                   std::size_t material_index = 0) {
    const auto* scene = dynamic_cast<const SceneAsset*>(FindAsset(result, "scene"));
    if (scene == nullptr) {
        return nullptr;
    }
    const auto* renderer = scene->scene.FindComponent(parcel::scene::kMeshRendererComponentType);
    if (renderer == nullptr) {
        return nullptr;
    }
    auto decoded =
        parcel::scene::ComponentCodec<MeshRendererComponent>::Decode(renderer->payloads[0]);
    if (!decoded || material_index >= decoded.Value().materials.size()) {
        return nullptr;
    }
    const AssetGuid guid = decoded.Value().materials[material_index];
    for (const parcel::import::CookedAsset& asset : result.assets) {
        if (asset.role == "material" && asset.guid == guid) {
            return dynamic_cast<const MaterialAsset*>(asset.object.get());
        }
    }
    return nullptr;
}

bool HasMeshRendererForEntity(const parcel::scene::SceneData& scene, std::string_view name) {
    const auto* chunk = scene.FindComponent(parcel::scene::kMeshRendererComponentType);
    if (chunk == nullptr) {
        return false;
    }
    for (const auto owner : chunk->owners) {
        const auto* entity = scene.FindEntity(owner);
        if (entity != nullptr && entity->name == name) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("glTF importer builds mesh material scene skin morph and animation") {
    GltfFixture fixture;
    auto importers = parcel::import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto* importer = importers.Value().FindForExtension(".gltf");
    REQUIRE(importer != nullptr);
    const auto settings = parcel::import::MakeDefaultImportSettings("gltf", "windows");
    auto imported = importer->Import(Request(fixture, "triangle.gltf", settings));
    REQUIRE(imported);
    CHECK(imported.Value().warnings.empty());
    CHECK(imported.Value().dependencies.size() == 1);

    const auto* mesh = dynamic_cast<const MeshAsset*>(FindAsset(imported.Value(), "mesh"));
    REQUIRE(mesh != nullptr);
    CHECK(mesh->vertices.size() == 3);
    CHECK(mesh->indices.size() == 3);
    CHECK(mesh->skin_vertices.size() == 3);
    CHECK(mesh->skin_vertices[0].joints[0] == 0);
    CHECK(mesh->skin_vertices[0].weights[0] > 0.7f);
    REQUIRE(mesh->morph_targets.size() == 1);
    CHECK(mesh->morph_targets[0].position_deltas[0] == parcel::Vec3{0.5f, 0.0f, 0.0f});
    CHECK(mesh->morph_weights.size() == 1);
    CHECK(mesh->morph_weights[0] == 0.0f);
    REQUIRE(mesh->sub_meshes.size() == 1);
    CHECK(mesh->sub_meshes[0].material_index == 0);

    const auto* material =
        dynamic_cast<const MaterialAsset*>(FindAsset(imported.Value(), "material"));
    REQUIRE(material != nullptr);
    CHECK(material->base_color_factor.x == Catch::Approx(0.2f));
    CHECK(material->metallic == Catch::Approx(0.3f));
    CHECK(material->roughness == Catch::Approx(0.7f));
    CHECK(material->alpha_mode == parcel::assets::AlphaMode::Mask);
    CHECK(material->double_sided);
    CHECK_FALSE(material->base_color_texture.texture.IsZero());
    CHECK(material->base_color_texture.sampler.wrap_s == parcel::assets::TextureWrap::Clamp);
    CHECK(material->base_color_texture.sampler.wrap_t == parcel::assets::TextureWrap::Mirror);
    CHECK(material->base_color_texture.sampler.min_filter ==
          parcel::assets::TextureFilter::Nearest);
    CHECK(material->base_color_texture.sampler.mag_filter ==
          parcel::assets::TextureFilter::Nearest);
    CHECK(material->base_color_texture.sampler.mip_filter ==
          parcel::assets::TextureMipFilter::Nearest);
    REQUIRE(material->normal_texture.IsValid());
    CHECK(material->normal_texture.sampler.wrap_s == parcel::assets::TextureWrap::Repeat);
    CHECK(material->normal_texture.sampler.wrap_t == parcel::assets::TextureWrap::Repeat);
    CHECK(material->normal_texture.sampler.min_filter == parcel::assets::TextureFilter::Linear);
    CHECK(material->normal_texture.sampler.mag_filter == parcel::assets::TextureFilter::Linear);
    CHECK(material->normal_texture.sampler.mip_filter == parcel::assets::TextureMipFilter::Linear);

    const auto* scene = dynamic_cast<const SceneAsset*>(FindAsset(imported.Value(), "scene"));
    REQUIRE(scene != nullptr);
    CHECK(scene->scene.entities.size() == 6);
    CHECK(scene->scene.entities[0].name == "Root");
    CHECK(scene->scene.entities[1].parent.value == 1);
    CHECK(scene->scene.entities[3].name == "MeshNode");
    CHECK(scene->scene.entities[3].parent.value == 1);

    const auto* clips = scene->scene.FindComponent(parcel::scene::kAnimationPlayerComponentType);
    REQUIRE(clips != nullptr);
    REQUIRE(clips->owners.size() == 1);
    CHECK(clips->owners[0].value == 1);
    auto decoded_clips =
        parcel::scene::ComponentCodec<AnimationPlayerComponent>::Decode(clips->payloads[0]);
    REQUIRE(decoded_clips);
    REQUIRE(decoded_clips.Value().clips.size() == 1);

    const auto* mesh_renderer =
        scene->scene.FindComponent(parcel::scene::kMeshRendererComponentType);
    REQUIRE(mesh_renderer != nullptr);
    auto decoded_renderer =
        parcel::scene::ComponentCodec<MeshRendererComponent>::Decode(mesh_renderer->payloads[0]);
    REQUIRE(decoded_renderer);
    CHECK(decoded_renderer.Value().mesh == FindCooked(imported.Value(), "mesh")->guid);
    CHECK(decoded_renderer.Value().materials.size() == 1);
    CHECK(decoded_renderer.Value().submesh_materials.size() == 1);
    CHECK(decoded_renderer.Value().submesh_materials[0] == 0);

    const auto* skin = scene->scene.FindComponent(parcel::scene::kSkinComponentType);
    REQUIRE(skin != nullptr);
    auto decoded_skin = parcel::scene::ComponentCodec<SkinComponent>::Decode(skin->payloads[0]);
    REQUIRE(decoded_skin);
    REQUIRE(decoded_skin.Value().joints.size() == 2);
    CHECK(decoded_skin.Value().joints[0].value == 2);
    CHECK(decoded_skin.Value().joints[1].value == 3);

    const auto* morph = scene->scene.FindComponent(parcel::scene::kMorphComponentType);
    REQUIRE(morph != nullptr);
    auto decoded_morph = parcel::scene::ComponentCodec<MorphComponent>::Decode(morph->payloads[0]);
    REQUIRE(decoded_morph);
    REQUIRE(decoded_morph.Value().weights.size() == 1);
    CHECK(decoded_morph.Value().weights[0] == 0.5f);

    const auto* camera = scene->scene.FindComponent(parcel::scene::kCameraComponentType);
    REQUIRE(camera != nullptr);
    auto decoded_camera =
        parcel::scene::ComponentCodec<CameraComponent>::Decode(camera->payloads[0]);
    REQUIRE(decoded_camera);
    CHECK(decoded_camera.Value().aspect == 1.5f);

    const auto* light = scene->scene.FindComponent(parcel::scene::kLightComponentType);
    REQUIRE(light != nullptr);
    auto decoded_light = parcel::scene::ComponentCodec<LightComponent>::Decode(light->payloads[0]);
    REQUIRE(decoded_light);
    CHECK(decoded_light.Value().type == parcel::scene::LightType::Point);
    CHECK(decoded_light.Value().intensity == 2.0f);

    const auto* clip =
        dynamic_cast<const AnimationClipAsset*>(FindAsset(imported.Value(), "animation"));
    REQUIRE(clip != nullptr);
    CHECK(clip->name == "Idle");
    CHECK(clip->channels.size() == 2);
    bool has_weights = false;
    bool has_translation = false;
    for (const auto& channel : clip->channels) {
        has_weights |= channel.path == parcel::assets::AnimationPath::Weights;
        has_translation |= channel.path == parcel::assets::AnimationPath::Translation;
        if (channel.path == parcel::assets::AnimationPath::Weights) {
            CHECK(channel.target_index == 0);
            CHECK(channel.values.size() == 2);
            CHECK(channel.values[1].x == 1.0f);
        }
    }
    CHECK(has_weights);
    CHECK(has_translation);
}

TEST_CASE("glTF importer resolves parent-relative external paths inside the project") {
    GltfFixture fixture;
    fixture.UseParentRelativePaths();
    auto importers = parcel::import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto* importer = importers.Value().FindForExtension(".gltf");
    REQUIRE(importer != nullptr);
    const auto settings = parcel::import::MakeDefaultImportSettings("gltf", "windows");

    auto imported = importer->Import(Request(fixture, "scenes/triangle.gltf", settings));
    REQUIRE(imported);
    CHECK(imported.Value().warnings.empty());
    const auto* mesh = dynamic_cast<const MeshAsset*>(FindAsset(imported.Value(), "mesh"));
    REQUIRE(mesh != nullptr);
    CHECK(mesh->vertices.size() == 3);
    const auto* material =
        dynamic_cast<const MaterialAsset*>(FindAsset(imported.Value(), "material"));
    REQUIRE(material != nullptr);
    CHECK_FALSE(material->base_color_texture.texture.IsZero());

    std::filesystem::remove(fixture.assets / "triangle.gltf");
    std::filesystem::remove(fixture.assets / "triangle.glb");
    std::filesystem::remove(fixture.assets / "triangle.bin");
    parcel::import::ImportOptions options;
    options.project_root = Utf8(fixture.project);
    options.source_root = Utf8(fixture.assets);
    options.output_root = Utf8(fixture.directory.Path() / "cooked");
    options.db_path = Utf8(fixture.directory.Path() / "import.db");
    options.target = "windows";
    options.jobs = 1;

    auto first = parcel::import::RunImport(options, importers.Value());
    REQUIRE(first);
    CHECK(first.Value().failed == 0);
    CHECK(first.Value().imported == 3);

    auto texture_meta = parcel::import::LoadAssetMetaFile(
        Utf8(fixture.assets / "textures" / "checker.tga.parcelmeta"));
    REQUIRE(texture_meta);
    REQUIRE(texture_meta.Value().assets.size() == 1);
    const AssetGuid texture_guid = texture_meta.Value().assets[0].guid;
    auto gltf_meta = parcel::import::LoadAssetMetaFile(
        Utf8(fixture.assets / "scenes" / "triangle.gltf.parcelmeta"));
    REQUIRE(gltf_meta);
    CHECK(std::find(gltf_meta.Value().dependencies.begin(), gltf_meta.Value().dependencies.end(),
                    texture_guid) != gltf_meta.Value().dependencies.end());

    auto second = parcel::import::RunImport(options, importers.Value());
    REQUIRE(second);
    CHECK(second.Value().failed == 0);
    CHECK(second.Value().imported == 0);
    CHECK(second.Value().skipped == 3);
}

TEST_CASE("glTF importer bakes KHR_texture_transform into UV0") {
    GltfFixture fixture;
    fixture.UseTextureTransform(true);
    auto importers = parcel::import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto* importer = importers.Value().FindForExtension(".gltf");
    REQUIRE(importer != nullptr);
    const auto settings = parcel::import::MakeDefaultImportSettings("gltf", "windows");

    auto imported = importer->Import(Request(fixture, "transform.gltf", settings));
    REQUIRE(imported);
    CHECK(imported.Value().warnings.empty());
    const auto* mesh = dynamic_cast<const MeshAsset*>(FindAsset(imported.Value(), "mesh"));
    REQUIRE(mesh != nullptr);
    REQUIRE(mesh->vertices.size() == 3);
    CHECK(mesh->vertices[0].texcoord0.x == Catch::Approx(0.25f));
    CHECK(mesh->vertices[0].texcoord0.y == Catch::Approx(0.5f));
    CHECK(mesh->vertices[1].texcoord0.x == Catch::Approx(0.25f));
    CHECK(mesh->vertices[1].texcoord0.y == Catch::Approx(2.5f));
    CHECK(mesh->vertices[2].texcoord0.x == Catch::Approx(-2.75f));
    CHECK(mesh->vertices[2].texcoord0.y == Catch::Approx(0.5f));
}

TEST_CASE("texture decode bakes KHR_texture_transform into texels") {
    const auto settings = parcel::import::MakeDefaultImportSettings("texture", "windows");
    parcel::import::TextureBakeOptions bake;
    bake.transform.offset = parcel::Vec2{0.5f, 0.0f};
    auto decoded = parcel::import::DecodeTextureAsset(HorizontalTga(), settings, "pattern", &bake);
    REQUIRE(decoded);
    const auto& texture = *decoded.Value();
    CHECK(texture.format == parcel::assets::TextureFormat::RGBA8);
    REQUIRE(texture.mips.size() == 3);
    REQUIRE(texture.mips[0].width == 4);
    REQUIRE(texture.mips[0].height == 1);
    REQUIRE(texture.mips[0].row_pitch == 16);

    const auto channel = [&](std::size_t x, std::size_t component) {
        return std::to_integer<int>(texture.mips[0].data[x * 4 + component]);
    };
    CHECK(channel(0, 0) == 0);
    CHECK(channel(0, 1) == 0);
    CHECK(channel(0, 2) == 255);
    CHECK(channel(0, 3) == 255);
    CHECK(channel(1, 0) == 255);
    CHECK(channel(1, 1) == 255);
    CHECK(channel(1, 2) == 255);
    CHECK(channel(2, 0) == 255);
    CHECK(channel(2, 1) == 0);
    CHECK(channel(2, 2) == 0);
    CHECK(channel(3, 0) == 0);
    CHECK(channel(3, 1) == 255);
    CHECK(channel(3, 2) == 0);
}

TEST_CASE("glTF importer bakes KHR_texture_transform into per-texture images") {
    GltfFixture fixture;
    fixture.UseTextureTransform(false);
    auto importers = parcel::import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto* importer = importers.Value().FindForExtension(".gltf");
    REQUIRE(importer != nullptr);
    const auto settings = parcel::import::MakeDefaultImportSettings("gltf", "windows");

    auto imported = importer->Import(Request(fixture, "transform.gltf", settings));
    REQUIRE(imported);
    CHECK(imported.Value().warnings.empty());
    const auto* mesh = dynamic_cast<const MeshAsset*>(FindAsset(imported.Value(), "mesh"));
    REQUIRE(mesh != nullptr);
    REQUIRE(mesh->vertices.size() == 3);
    CHECK(mesh->vertices[0].texcoord0 == parcel::Vec2{0.0f, 0.0f});
    CHECK(mesh->vertices[1].texcoord0 == parcel::Vec2{1.0f, 0.0f});
    CHECK(mesh->vertices[2].texcoord0 == parcel::Vec2{0.0f, 1.0f});

    const auto* material = SceneMaterial(imported.Value());
    REQUIRE(material != nullptr);
    CHECK(material->base_color_texture.uv_set == 0);
    CHECK(material->normal_texture.uv_set == 0);
    CHECK(material->base_color_texture.texture != material->normal_texture.texture);
    const auto* baked_asset = FindCooked(imported.Value(), "texture");
    REQUIRE(baked_asset != nullptr);
    CHECK(material->base_color_texture.texture == baked_asset->guid);

    const auto* baked_texture =
        dynamic_cast<const parcel::assets::TextureAsset*>(baked_asset->object.get());
    REQUIRE(baked_texture != nullptr);
    CHECK(baked_texture->sRGB);
    REQUIRE(baked_texture->mips.size() == 1);
    CHECK(baked_texture->mips[0].width == 1);
    CHECK(baked_texture->mips[0].height == 1);
}

TEST_CASE("glTF importer selects KHR_materials_variants") {
    GltfFixture fixture;
    fixture.UseMaterialVariants();
    auto importers = parcel::import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto* importer = importers.Value().FindForExtension(".gltf");
    REQUIRE(importer != nullptr);

    const auto import_with_variant = [&](std::string_view variant) {
        auto settings = parcel::import::MakeDefaultImportSettings("gltf", "windows");
        for (auto& [key, value] : settings.object_value) {
            if (key == "materialVariant") {
                value.string_value.assign(variant);
            }
        }
        return importer->Import(Request(fixture, "variants.gltf", settings));
    };

    auto default_import = import_with_variant("");
    REQUIRE(default_import);
    CHECK(default_import.Value().warnings.empty());
    const MaterialAsset* default_material = SceneMaterial(default_import.Value());
    REQUIRE(default_material != nullptr);
    CHECK(default_material->name == "MorphMaterial");

    auto blue_import = import_with_variant("Blue");
    REQUIRE(blue_import);
    const MaterialAsset* blue_material = SceneMaterial(blue_import.Value());
    REQUIRE(blue_material != nullptr);
    CHECK(blue_material->name == "VariantBlue");
    CHECK(blue_material->base_color_factor.x == Catch::Approx(0.1f));
    CHECK(blue_material->base_color_factor.z == Catch::Approx(0.9f));

    auto red_import = import_with_variant("Red");
    REQUIRE(red_import);
    const MaterialAsset* red_material = SceneMaterial(red_import.Value());
    REQUIRE(red_material != nullptr);
    CHECK(red_material->name == "MorphMaterial");

    auto missing_import = import_with_variant("Missing");
    REQUIRE_FALSE(missing_import);
    CHECK(missing_import.GetError().code == parcel::core::ErrorCode::InvalidArgument);
}

TEST_CASE("glTF importer selects default and named scenes") {
    GltfFixture fixture;
    fixture.UseScenes();
    auto importers = parcel::import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto* importer = importers.Value().FindForExtension(".gltf");
    REQUIRE(importer != nullptr);

    const auto import_with_scene = [&](std::string_view scene) {
        auto settings = parcel::import::MakeDefaultImportSettings("gltf", "windows");
        for (auto& [key, value] : settings.object_value) {
            if (key == "scene") {
                value.string_value.assign(scene);
            }
        }
        return importer->Import(Request(fixture, "scenes.gltf", settings));
    };

    auto default_import = import_with_scene("");
    REQUIRE(default_import);
    CHECK(default_import.Value().warnings.empty());
    const auto* default_scene =
        dynamic_cast<const SceneAsset*>(FindAsset(default_import.Value(), "scene"));
    REQUIRE(default_scene != nullptr);
    CHECK(default_scene->scene.entities.size() == 7);
    CHECK(HasMeshRendererForEntity(default_scene->scene, "MeshNode"));
    CHECK_FALSE(HasMeshRendererForEntity(default_scene->scene, "ExtraNode"));

    auto extra_import = import_with_scene("Extra");
    REQUIRE(extra_import);
    const auto* extra_scene =
        dynamic_cast<const SceneAsset*>(FindAsset(extra_import.Value(), "scene"));
    REQUIRE(extra_scene != nullptr);
    CHECK(extra_scene->scene.entities.size() == 7);
    CHECK(HasMeshRendererForEntity(extra_scene->scene, "ExtraNode"));
    CHECK_FALSE(HasMeshRendererForEntity(extra_scene->scene, "MeshNode"));

    auto indexed_import = import_with_scene("1");
    REQUIRE(indexed_import);
    const auto* indexed_scene =
        dynamic_cast<const SceneAsset*>(FindAsset(indexed_import.Value(), "scene"));
    REQUIRE(indexed_scene != nullptr);
    CHECK(HasMeshRendererForEntity(indexed_scene->scene, "ExtraNode"));
    CHECK_FALSE(HasMeshRendererForEntity(indexed_scene->scene, "MeshNode"));

    auto missing_import = import_with_scene("Missing");
    REQUIRE_FALSE(missing_import);
    CHECK(missing_import.GetError().code == parcel::core::ErrorCode::InvalidArgument);
}

TEST_CASE("glTF importer decodes EXT_meshopt_compression") {
    GltfFixture fixture;
    fixture.UseMeshoptCompression();
    auto importers = parcel::import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto* importer = importers.Value().FindForExtension(".gltf");
    REQUIRE(importer != nullptr);
    const auto settings = parcel::import::MakeDefaultImportSettings("gltf", "windows");
    auto imported = importer->Import(Request(fixture, "meshopt.gltf", settings));
    REQUIRE(imported);
    CHECK(imported.Value().warnings.empty());

    const auto* mesh = dynamic_cast<const MeshAsset*>(FindAsset(imported.Value(), "mesh"));
    REQUIRE(mesh != nullptr);
    REQUIRE(mesh->vertices.size() == 3);
    bool has_origin = false;
    bool has_x = false;
    bool has_y = false;
    for (const auto& vertex : mesh->vertices) {
        has_origin = has_origin || (vertex.position.x == Catch::Approx(0.0f) &&
                                    vertex.position.y == Catch::Approx(0.0f));
        has_x = has_x || (vertex.position.x == Catch::Approx(1.0f) &&
                          vertex.position.y == Catch::Approx(0.0f));
        has_y = has_y || (vertex.position.x == Catch::Approx(0.0f) &&
                          vertex.position.y == Catch::Approx(1.0f));
    }
    CHECK(has_origin);
    CHECK(has_x);
    CHECK(has_y);
    auto indices = mesh->indices;
    std::sort(indices.begin(), indices.end());
    CHECK(indices == std::vector<std::uint32_t>{0, 1, 2});
}

TEST_CASE("glTF importer decodes embedded images") {
    for (const bool data_uri : {false, true}) {
        INFO("data URI image: " << data_uri);
        GltfFixture fixture;
        fixture.UseEmbeddedImage(data_uri);
        auto importers = parcel::import::CreateStandardImporterRegistry();
        REQUIRE(importers);
        const auto* importer = importers.Value().FindForExtension(".gltf");
        REQUIRE(importer != nullptr);
        const auto settings = parcel::import::MakeDefaultImportSettings("gltf", "windows");
        auto imported = importer->Import(Request(fixture, "triangle.gltf", settings));
        REQUIRE(imported);
        CHECK(imported.Value().warnings.empty());

        const auto* texture = dynamic_cast<const parcel::assets::TextureAsset*>(
            FindAsset(imported.Value(), "texture"));
        REQUIRE(texture != nullptr);
        CHECK(texture->format == parcel::assets::TextureFormat::RGBA8);
        CHECK(texture->sRGB);
        REQUIRE(texture->mips.size() == 1);
        CHECK(texture->mips[0].width == 1);
        CHECK(texture->mips[0].height == 1);

        const auto* cooked_texture = FindCooked(imported.Value(), "texture");
        REQUIRE(cooked_texture != nullptr);
        CHECK_FALSE(cooked_texture->guid.IsZero());
        CHECK(cooked_texture->local_id == "texture:gltf:0");
        CHECK(std::find(imported.Value().dependencies.begin(), imported.Value().dependencies.end(),
                        cooked_texture->guid) != imported.Value().dependencies.end());

        const auto* material =
            dynamic_cast<const MaterialAsset*>(FindAsset(imported.Value(), "material"));
        REQUIRE(material != nullptr);
        CHECK(material->base_color_texture.texture == cooked_texture->guid);
    }
}

TEST_CASE("glTF import pipeline cooks embedded texture subassets") {
    for (const bool data_uri : {false, true}) {
        INFO("data URI image: " << data_uri);
        GltfFixture fixture;
        fixture.UseEmbeddedImage(data_uri);
        std::filesystem::remove(fixture.assets / "textures" / "checker.tga");
        std::filesystem::remove(fixture.assets / "triangle.glb");

        auto importers = parcel::import::CreateStandardImporterRegistry();
        REQUIRE(importers);
        parcel::import::ImportOptions options;
        options.project_root = Utf8(fixture.project);
        options.source_root = Utf8(fixture.assets);
        options.output_root = Utf8(fixture.directory.Path() / "cooked");
        options.db_path = Utf8(fixture.directory.Path() / "import.db");
        options.target = "windows";
        options.jobs = 1;

        auto first = parcel::import::RunImport(options, importers.Value());
        REQUIRE(first);
        CHECK(first.Value().failed == 0);
        CHECK(first.Value().imported == 2);
        CHECK(first.Value().skipped == 0);

        auto meta =
            parcel::import::LoadAssetMetaFile(Utf8(fixture.assets / "triangle.gltf.parcelmeta"));
        REQUIRE(meta);
        CHECK(meta.Value().assets.size() == 5);
        const auto texture_asset = std::find_if(
            meta.Value().assets.begin(), meta.Value().assets.end(),
            [](const parcel::import::MetaAsset& asset) { return asset.role == "texture"; });
        REQUIRE(texture_asset != meta.Value().assets.end());
        CHECK(texture_asset->local_id == "texture:gltf:0");
        CHECK(std::find(meta.Value().dependencies.begin(), meta.Value().dependencies.end(),
                        texture_asset->guid) != meta.Value().dependencies.end());
        CHECK(std::filesystem::is_regular_file(
            Utf8(fixture.directory.Path() / "cooked" / (texture_asset->guid.ToHex() + ".prca"))));

        auto second = parcel::import::RunImport(options, importers.Value());
        REQUIRE(second);
        CHECK(second.Value().failed == 0);
        CHECK(second.Value().imported == 0);
        CHECK(second.Value().skipped == 2);

        parcel::pack::PackConfig pack;
        pack.project_root = Utf8(fixture.project);
        pack.cooked_root = options.output_root;
        pack.output_root = Utf8(fixture.directory.Path() / "build");
        pack.package_name = "embedded";
        pack.package_version = "1.0.0";
        pack.target = "windows";
        pack.jobs = 1;
        parcel::pack::CollectorConfig collector;
        collector.name = "main";
        collector.type = parcel::pack::CollectorType::Main;
        pack.collectors.push_back(std::move(collector));
        auto packed = parcel::pack::RunPack(pack);
        REQUIRE(packed);
        CHECK(packed.Value().report.errors.empty());
        CHECK(packed.Value().report.asset_count == 6);
        CHECK(std::any_of(packed.Value().report.assets.begin(), packed.Value().report.assets.end(),
                          [&](const parcel::pack::PackAssetReport& asset) {
                              return asset.guid == texture_asset->guid;
                          }));
    }
}

TEST_CASE("glTF importer applies scene sidecar overrides") {
    GltfFixture fixture;
    std::filesystem::create_directories(fixture.assets / "settings");
    WriteText(fixture.assets / "settings" / "pipeline.parcel", "");
    WriteText(fixture.assets / "settings" / "frame.parcel", "");
    WriteText(fixture.assets / "triangle.gltf.parcel.json", R"json({
  "version": 1,
  "settings": {
    "pipeline": "assets/settings/pipeline.parcel",
    "frame": "assets/settings/frame.parcel"
  },
  "cameras": [{"name": "Camera", "yfov": 0.5, "aspect": 1.25}],
  "lights": [{"name": "Light", "type": "spot", "intensity": 3.5, "range": 8.0}]
})json");

    auto importers = parcel::import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto* importer = importers.Value().FindForExtension(".gltf");
    REQUIRE(importer != nullptr);
    const auto settings = parcel::import::MakeDefaultImportSettings("gltf", "windows");
    auto imported = importer->Import(Request(fixture, "triangle.gltf", settings));
    REQUIRE(imported);
    const auto* scene = dynamic_cast<const SceneAsset*>(FindAsset(imported.Value(), "scene"));
    REQUIRE(scene != nullptr);
    CHECK(scene->scene.pipeline_settings ==
          parcel::import::MakeSourceGuid("assets/settings/pipeline.parcel"));
    CHECK(scene->scene.frame_settings ==
          parcel::import::MakeSourceGuid("assets/settings/frame.parcel"));

    const auto* camera = scene->scene.FindComponent(parcel::scene::kCameraComponentType);
    REQUIRE(camera != nullptr);
    auto decoded_camera =
        parcel::scene::ComponentCodec<CameraComponent>::Decode(camera->payloads[0]);
    REQUIRE(decoded_camera);
    CHECK(decoded_camera.Value().yfov == 0.5f);
    CHECK(decoded_camera.Value().aspect == 1.25f);

    const auto* light = scene->scene.FindComponent(parcel::scene::kLightComponentType);
    REQUIRE(light != nullptr);
    auto decoded_light = parcel::scene::ComponentCodec<LightComponent>::Decode(light->payloads[0]);
    REQUIRE(decoded_light);
    CHECK(decoded_light.Value().type == parcel::scene::LightType::Spot);
    CHECK(decoded_light.Value().intensity == 3.5f);
    CHECK(decoded_light.Value().range == 8.0f);
}

TEST_CASE("glTF importer applies the scale setting to mesh nodes and animation") {
    GltfFixture fixture;
    auto importers = parcel::import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto* importer = importers.Value().FindForExtension(".gltf");
    REQUIRE(importer != nullptr);
    auto settings = parcel::import::MakeDefaultImportSettings("gltf", "windows");
    for (auto& [key, value] : settings.object_value) {
        if (key == "scale") {
            value.number_value = 2.0;
        }
    }
    auto imported = importer->Import(Request(fixture, "triangle.gltf", settings));
    REQUIRE(imported);
    const auto* mesh = dynamic_cast<const MeshAsset*>(FindAsset(imported.Value(), "mesh"));
    REQUIRE(mesh != nullptr);
    CHECK(mesh->vertices[1].position.x == 2.0f);
    REQUIRE(mesh->morph_targets.size() == 1);
    CHECK(mesh->morph_targets[0].position_deltas[0].x == 1.0f);
    const auto* scene = dynamic_cast<const SceneAsset*>(FindAsset(imported.Value(), "scene"));
    REQUIRE(scene != nullptr);
    CHECK(scene->scene.entities[0].transform.position.x == 2.0f);
    const auto* clip =
        dynamic_cast<const AnimationClipAsset*>(FindAsset(imported.Value(), "animation"));
    REQUIRE(clip != nullptr);
    for (const auto& channel : clip->channels) {
        if (channel.path == parcel::assets::AnimationPath::Translation) {
            CHECK(channel.values[1].x == 2.0f);
        }
    }
}

TEST_CASE("glTF import pipeline shares canonical textures and skips unchanged sources") {
    GltfFixture fixture;
    std::filesystem::remove(fixture.assets / "triangle.glb");
    auto importers = parcel::import::CreateStandardImporterRegistry();
    REQUIRE(importers);

    parcel::import::ImportOptions options;
    options.project_root = Utf8(fixture.project);
    options.source_root = Utf8(fixture.assets);
    options.output_root = Utf8(fixture.directory.Path() / "cooked");
    options.db_path = Utf8(fixture.directory.Path() / "import.db");
    options.target = "windows";
    options.jobs = 2;

    auto first = parcel::import::RunImport(options, importers.Value());
    REQUIRE(first);
    CHECK(first.Value().failed == 0);
    CHECK(first.Value().imported == 3);
    CHECK(first.Value().skipped == 0);

    auto texture_meta = parcel::import::LoadAssetMetaFile(
        Utf8(fixture.assets / "textures" / "checker.tga.parcelmeta"));
    REQUIRE(texture_meta);
    REQUIRE(texture_meta.Value().assets.size() == 1);
    const AssetGuid texture_guid = texture_meta.Value().assets[0].guid;

    auto gltf_meta =
        parcel::import::LoadAssetMetaFile(Utf8(fixture.assets / "triangle.gltf.parcelmeta"));
    REQUIRE(gltf_meta);
    CHECK(gltf_meta.Value().importer == "gltf");
    CHECK(std::find(gltf_meta.Value().dependencies.begin(), gltf_meta.Value().dependencies.end(),
                    texture_guid) != gltf_meta.Value().dependencies.end());
    CHECK(gltf_meta.Value().assets.size() == 4);

    auto second = parcel::import::RunImport(options, importers.Value());
    REQUIRE(second);
    CHECK(second.Value().failed == 0);
    CHECK(second.Value().imported == 0);
    CHECK(second.Value().skipped == 3);
}

TEST_CASE("glTF importer reads GLB binary buffers") {
    GltfFixture fixture;
    auto importers = parcel::import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto* importer = importers.Value().FindForExtension(".glb");
    REQUIRE(importer != nullptr);
    const auto settings = parcel::import::MakeDefaultImportSettings("gltf", "windows");
    auto imported = importer->Import(Request(fixture, "triangle.glb", settings));
    REQUIRE(imported);
    const auto* mesh = dynamic_cast<const MeshAsset*>(FindAsset(imported.Value(), "mesh"));
    REQUIRE(mesh != nullptr);
    CHECK(mesh->vertices.size() == 3);
    CHECK(mesh->morph_targets.size() == 1);
}

TEST_CASE("glTF importer rejects missing and malformed sources") {
    GltfFixture fixture;
    auto importers = parcel::import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto* importer = importers.Value().FindForExtension(".gltf");
    REQUIRE(importer != nullptr);
    const auto settings = parcel::import::MakeDefaultImportSettings("gltf", "windows");
    auto missing = importer->Import(Request(fixture, "missing.gltf", settings));
    CHECK_FALSE(missing);

    WriteText(fixture.assets / "broken.gltf", "{not-json");
    auto broken = importer->Import(Request(fixture, "broken.gltf", settings));
    CHECK_FALSE(broken);

    std::string outside = TriangleGltf(true);
    const std::string relative_buffer = "\"uri\": \"triangle.bin\"";
    const auto buffer_position = outside.find(relative_buffer);
    REQUIRE(buffer_position != std::string::npos);
    outside.replace(buffer_position, relative_buffer.size(), "\"uri\": \"../../outside.bin\"");
    WriteText(fixture.assets / "outside.gltf", outside);
    auto escaped = importer->Import(Request(fixture, "outside.gltf", settings));
    CHECK_FALSE(escaped);
}
