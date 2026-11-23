#include <catch_amalgamated.hpp>

#include "parcel/assets/animation_clip_asset.hpp"
#include "parcel/assets/asset_codec.hpp"
#include "parcel/assets/ies_profile_asset.hpp"
#include "parcel/assets/json.hpp"
#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/mesh_asset.hpp"
#include "parcel/assets/scene_asset.hpp"
#include "parcel/assets/serializers.hpp"
#include "parcel/assets/shader_library_asset.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/platform_file.hpp"
#include "parcel/import/content_hash.hpp"
#include "parcel/import/ies_profile.hpp"
#include "parcel/import/import_db.hpp"
#include "parcel/import/importers.hpp"
#include "parcel/import/meta.hpp"
#include "parcel/import/pipeline.hpp"
#include "parcel/scene/component_codec.hpp"
#include "parcel/scene/scene_data.hpp"
#include "parcel/serialize/asset_file.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace parcel;
using parcel::assets::AssetObject;
using parcel::assets::TypeRegistry;
using parcel::core::AssetGuid;
using parcel::core::Hash256;
using parcel::test::TempDir;
using parcel::test::ToUtf8;

namespace {

std::vector<std::byte> Bytes(std::string_view text) {
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                  reinterpret_cast<const std::byte*>(text.data()) + text.size());
}

std::vector<std::byte> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<std::byte> result(size);
    file.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(size));
    REQUIRE(file);
    return result;
}

void WriteFile(const std::filesystem::path& path, std::span<const std::byte> data) {
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    REQUIRE(file);
}

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(file);
}

std::uint32_t Crc32(std::span<const std::uint8_t> data) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t value : data) {
        crc ^= value;
        for (int i = 0; i < 8; ++i) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

std::uint32_t Adler32(std::span<const std::uint8_t> data) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t value : data) {
        a = (a + value) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16U) | a;
}

void AppendU32Be(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
    out.push_back(static_cast<std::uint8_t>(value >> 16U));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
}

void AppendChunk(std::vector<std::uint8_t>& out, std::string_view type,
                 std::span<const std::uint8_t> data) {
    AppendU32Be(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t type_start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), data.begin(), data.end());
    const std::uint32_t crc =
        Crc32(std::span<const std::uint8_t>(out.data() + type_start, out.size() - type_start));
    AppendU32Be(out, crc);
}

void WritePng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
              std::span<const std::uint8_t> rgba) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (width * 4U + 1U));
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const auto offset = static_cast<std::size_t>(y) * width * 4U;
        raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(offset),
                   rgba.begin() + static_cast<std::ptrdiff_t>(offset + width * 4U));
    }

    std::vector<std::uint8_t> zlib = {0x78, 0x01, 0x01};
    const std::uint16_t length = static_cast<std::uint16_t>(raw.size());
    zlib.push_back(static_cast<std::uint8_t>(length));
    zlib.push_back(static_cast<std::uint8_t>(length >> 8U));
    const std::uint16_t inverse = static_cast<std::uint16_t>(~length);
    zlib.push_back(static_cast<std::uint8_t>(inverse));
    zlib.push_back(static_cast<std::uint8_t>(inverse >> 8U));
    zlib.insert(zlib.end(), raw.begin(), raw.end());
    AppendU32Be(zlib, Adler32(raw));

    std::vector<std::uint8_t> file = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<std::uint8_t> ihdr;
    AppendU32Be(ihdr, width);
    AppendU32Be(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    AppendChunk(file, "IHDR", ihdr);
    AppendChunk(file, "IDAT", zlib);
    AppendChunk(file, "IEND", {});

    std::ofstream out(path, std::ios::binary);
    REQUIRE(out);
    out.write(reinterpret_cast<const char*>(file.data()),
              static_cast<std::streamsize>(file.size()));
    REQUIRE(out);
}

void WriteTga(const std::filesystem::path& path, std::uint16_t width, std::uint16_t height,
              std::span<const std::uint8_t> rgba) {
    std::vector<std::uint8_t> file(18, 0);
    file[2] = 2;
    file[12] = static_cast<std::uint8_t>(width);
    file[13] = static_cast<std::uint8_t>(width >> 8U);
    file[14] = static_cast<std::uint8_t>(height);
    file[15] = static_cast<std::uint8_t>(height >> 8U);
    file[16] = 32;
    file[17] = 8;
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        file.push_back(rgba[i + 2]);
        file.push_back(rgba[i + 1]);
        file.push_back(rgba[i]);
        file.push_back(rgba[i + 3]);
    }
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out);
    out.write(reinterpret_cast<const char*>(file.data()),
              static_cast<std::streamsize>(file.size()));
    REQUIRE(out);
}

void WriteHdr(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
              std::span<const std::uint8_t> rgbe) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out);
    out << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " << height << " +X " << width << "\n";
    out.write(reinterpret_cast<const char*>(rgbe.data()),
              static_cast<std::streamsize>(rgbe.size()));
    REQUIRE(out);
}

struct ImportFixture {
    TempDir directory{"import"};
    std::filesystem::path project = directory.Path() / "project";
    std::filesystem::path source = project / "assets";
    std::filesystem::path output = directory.Path() / "cooked";
    std::filesystem::path db = directory.Path() / "import.db";

    ImportFixture() { std::filesystem::create_directories(source); }

    parcel::import::ImportOptions Options() const {
        parcel::import::ImportOptions options;
        options.project_root = ToUtf8(project);
        options.source_root = ToUtf8(source);
        options.output_root = ToUtf8(output);
        options.db_path = ToUtf8(db);
        return options;
    }
};

std::shared_ptr<AssetObject> DecodeOutput(const std::filesystem::path& path,
                                          const TypeRegistry& registry) {
    const auto bytes = ReadFile(path);
    auto decoded = assets::DecodeAssetBytes(bytes, registry);
    REQUIRE(decoded);
    return decoded.Value();
}

void WriteTextureMeta(const std::filesystem::path& source, std::string_view relative,
                      std::string_view settings_text) {
    auto settings = assets::ParseJson(settings_text);
    REQUIRE(settings);
    import::AssetMeta meta;
    meta.source_guid = import::MakeSourceGuid(relative);
    meta.importer = "texture";
    meta.importer_version = 3;
    meta.settings = std::move(settings).Value();
    REQUIRE(import::WriteAssetMetaFile(ToUtf8(source) + ".parcelmeta", meta));
}

std::shared_ptr<assets::TextureAsset> DecodeTextureOutput(const ImportFixture& fixture,
                                                          const std::filesystem::path& source) {
    auto meta = import::LoadAssetMetaFile(ToUtf8(source) + ".parcelmeta");
    REQUIRE(meta);
    REQUIRE(meta.Value().assets.size() == 1);
    TypeRegistry registry;
    REQUIRE(assets::RegisterStandardAssetSerializers(registry));
    const auto path = fixture.output / (meta.Value().assets[0].guid.ToHex() + ".prca");
    auto texture = std::static_pointer_cast<assets::TextureAsset>(DecodeOutput(path, registry));
    REQUIRE(texture);
    return texture;
}

std::array<std::uint8_t, 3> Decode565(std::uint16_t value) {
    const std::uint8_t r = static_cast<std::uint8_t>((value >> 11U) & 0x1fU);
    const std::uint8_t g = static_cast<std::uint8_t>((value >> 5U) & 0x3fU);
    const std::uint8_t b = static_cast<std::uint8_t>(value & 0x1fU);
    return {static_cast<std::uint8_t>((r << 3U) | (r >> 2U)),
            static_cast<std::uint8_t>((g << 2U) | (g >> 4U)),
            static_cast<std::uint8_t>((b << 3U) | (b >> 2U))};
}

std::array<std::uint8_t, 3> DecodeBc1Pixel(std::span<const std::byte> data, std::size_t offset,
                                           std::size_t pixel) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data() + offset);
    const std::uint16_t c0 = static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8U));
    const std::uint16_t c1 = static_cast<std::uint16_t>(bytes[2] | (bytes[3] << 8U));
    const std::uint8_t code = static_cast<std::uint8_t>((bytes[4] >> (pixel * 2U)) & 0x03U);
    const auto e0 = Decode565(c0);
    const auto e1 = Decode565(c1);
    if (code == 0) {
        return e0;
    }
    return e1;
}

std::uint8_t DecodeBc4Pixel(std::span<const std::byte> data, std::size_t offset,
                            std::size_t pixel) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data() + offset);
    const std::uint64_t indices = static_cast<std::uint64_t>(bytes[2]) |
                                  (static_cast<std::uint64_t>(bytes[3]) << 8U) |
                                  (static_cast<std::uint64_t>(bytes[4]) << 16U) |
                                  (static_cast<std::uint64_t>(bytes[5]) << 24U) |
                                  (static_cast<std::uint64_t>(bytes[6]) << 32U) |
                                  (static_cast<std::uint64_t>(bytes[7]) << 40U);
    const std::uint8_t code = static_cast<std::uint8_t>((indices >> (pixel * 3U)) & 0x07U);
    const std::uint8_t e0 = bytes[0];
    const std::uint8_t e1 = bytes[1];
    if (code == 0) {
        return e0;
    }
    if (code == 1) {
        return e1;
    }
    return e0;
}

std::vector<std::uint8_t> SolidPixels(std::uint32_t width, std::uint32_t height,
                                      std::array<std::uint8_t, 4> rgba) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
    for (std::size_t i = 0; i < pixels.size(); i += 4U) {
        pixels[i] = rgba[0];
        pixels[i + 1U] = rgba[1];
        pixels[i + 2U] = rgba[2];
        pixels[i + 3U] = rgba[3];
    }
    return pixels;
}

} // namespace

TEST_CASE("asset meta serializes stable GUIDs and hashes") {
    import::AssetMeta meta;
    meta.source_guid = import::MakeSourceGuid("Assets/Texture.png");
    meta.importer = "texture";
    meta.importer_version = 1;
    meta.settings.type = assets::JsonType::Object;
    meta.settings_hash = import::HashSettings(meta.settings);
    meta.source_hash = core::Sha256("source");
    meta.assets.push_back({import::MakeSubAssetGuid(meta.source_guid, "texture", "texture"),
                           "TextureAsset", "texture", "texture"});
    meta.dependencies.push_back(import::MakeSourceGuid("Assets/Other.png"));

    const std::string text = import::SerializeAssetMeta(meta);
    auto parsed = import::ParseAssetMeta(text);
    REQUIRE(parsed);
    CHECK(parsed.Value().source_guid == meta.source_guid);
    CHECK(parsed.Value().source_hash == meta.source_hash);
    CHECK(parsed.Value().settings_hash == meta.settings_hash);
    REQUIRE(parsed.Value().assets.size() == 1);
    CHECK(parsed.Value().assets[0].guid == meta.assets[0].guid);
    CHECK(parsed.Value().assets[0].local_id == "texture");

    CHECK(import::MakeSourceGuid("Assets/Texture.png") == meta.source_guid);
    CHECK(import::MakeSourceGuid("Assets/Other.png") != meta.source_guid);
    CHECK(import::MakeSubAssetGuid(meta.source_guid, "texture", "texture") == meta.assets[0].guid);

    TempDir directory("meta");
    const auto path = directory.Path() / "texture.png.parcelmeta";
    REQUIRE(import::WriteAssetMetaFile(ToUtf8(path), meta));
    auto loaded = import::LoadAssetMetaFile(ToUtf8(path));
    REQUIRE(loaded);
    CHECK(loaded.Value().source_guid == meta.source_guid);
}

TEST_CASE("asset meta rejects invalid identifiers and versions") {
    CHECK_FALSE(import::ParseAssetMeta("{}"));
    CHECK_FALSE(import::ParseAssetMeta(R"({"version": 2})"));
    CHECK_FALSE(import::ParseAssetMeta(
        R"({"version":1,"sourceGuid":"00000000000000000000000000000000","importer":"x","importerVersion":1})"));
    CHECK_FALSE(import::ParseAssetMeta(
        R"({"version":1,"sourceGuid":"00112233445566778899aabbccddeeff","importer":"x","importerVersion":1,"assets":[{"guid":"00000000000000000000000000000000"}]})"));
}

TEST_CASE("json serializer escapes strings and preserves structure") {
    assets::JsonValue object;
    object.type = assets::JsonType::Object;
    assets::JsonValue text;
    text.type = assets::JsonType::String;
    text.string_value = "a\n\"b\"";
    object.object_value.emplace_back("text", std::move(text));
    assets::JsonValue array;
    array.type = assets::JsonType::Array;
    array.array_value.push_back(assets::JsonValue{});
    object.object_value.emplace_back("array", std::move(array));

    const std::string encoded = assets::SerializeJson(object);
    CHECK(encoded.find("\\n") != std::string::npos);
    CHECK(encoded.find("\\\"b\\\"") != std::string::npos);
    auto decoded = assets::ParseJson(encoded);
    REQUIRE(decoded);
    CHECK(decoded.Value().Find("text")->string_value == "a\n\"b\"");
    REQUIRE(decoded.Value().Find("array")->array_value.size() == 1);

    const auto with_bom = assets::ParseJson(std::string("\xEF\xBB\xBF{\"ok\":true}"));
    REQUIRE(with_bom);
    CHECK(with_bom.Value().Find("ok")->bool_value);
}

TEST_CASE("import DB round-trips and is byte deterministic") {
    TempDir directory("db");
    import::ImportDB db;
    import::ImportDBRecord record;
    record.source_path = "assets/a.png";
    record.source_guid = import::MakeSourceGuid(record.source_path);
    record.importer = "texture";
    record.importer_version = 1;
    record.source_hash = core::Sha256("source");
    record.settings_hash = core::Sha256("settings");
    record.texture_index_hash = core::Sha256("texture-index");
    record.outputs.push_back({import::MakeSubAssetGuid(record.source_guid, "texture", "texture"),
                              core::Sha256("output")});
    record.dependencies.push_back({import::MakeSourceGuid("assets/b.png"), core::Sha256("dep")});
    db.records.push_back(record);

    const auto path = directory.Path() / "import.db";
    REQUIRE(import::SaveImportDB(ToUtf8(path), db));
    const auto bytes = ReadFile(path);
    auto loaded = import::LoadImportDB(ToUtf8(path));
    REQUIRE(loaded);
    REQUIRE(loaded.Value().records.size() == 1);
    CHECK(loaded.Value().records[0].source_path == "assets/a.png");
    CHECK(loaded.Value().records[0].outputs[0].output_hash == core::Sha256("output"));
    CHECK(loaded.Value().records[0].texture_index_hash == core::Sha256("texture-index"));

    REQUIRE(import::SaveImportDB(ToUtf8(path), loaded.Value()));
    CHECK(ReadFile(path) == bytes);

    auto missing = import::LoadImportDB(ToUtf8(directory.Path() / "missing.db"));
    REQUIRE(missing);
    CHECK(missing.Value().records.empty());

    const auto corrupt_path = directory.Path() / "corrupt.db";
    WriteTextFile(corrupt_path, "not-a-database");
    auto corrupt = import::LoadImportDB(ToUtf8(corrupt_path));
    CHECK_FALSE(corrupt);
    CHECK(corrupt.GetError().code == core::ErrorCode::Corrupt);
}

TEST_CASE("import pipeline decodes PNG and TGA and skips unchanged sources") {
    ImportFixture fixture;
    const std::array<std::uint8_t, 16> pixels = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
    };
    WritePng(fixture.source / "red.png", 2, 2, pixels);
    WriteTga(fixture.source / "blue.tga", 2, 2, pixels);

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto options = fixture.Options();
    auto first = import::RunImport(options, importers.Value());
    REQUIRE(first);
    CHECK(first.Value().imported == 2);
    CHECK(first.Value().skipped == 0);
    CHECK(first.Value().failed == 0);
    CHECK(std::filesystem::exists(fixture.output / "import_report.json"));

    auto meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "red.png.parcelmeta"));
    REQUIRE(meta);
    REQUIRE(meta.Value().assets.size() == 1);
    const auto output_path = fixture.output / (meta.Value().assets[0].guid.ToHex() + ".prca");
    REQUIRE(std::filesystem::exists(output_path));

    TypeRegistry registry;
    REQUIRE(assets::RegisterStandardAssetSerializers(registry));
    auto object = DecodeOutput(output_path, registry);
    auto texture = std::static_pointer_cast<assets::TextureAsset>(object);
    REQUIRE(texture);
    CHECK(texture->format == assets::TextureFormat::RGBA8);
    CHECK(texture->sRGB);
    CHECK(texture->mip_count == 2);
    REQUIRE(texture->mips.size() == 2);
    CHECK(texture->mips[0].width == 2);
    CHECK(texture->mips[0].height == 2);
    CHECK(texture->mips[0].row_pitch == 8);
    CHECK(texture->mips[1].width == 1);
    CHECK(texture->mips[1].height == 1);

    auto second = import::RunImport(options, importers.Value());
    REQUIRE(second);
    CHECK(second.Value().imported == 0);
    CHECK(second.Value().skipped == 2);
    CHECK(second.Value().failed == 0);
}

TEST_CASE("texture importer converts BGRA8 and floating point formats") {
    ImportFixture fixture;
    const std::vector<std::uint8_t> red = SolidPixels(1, 1, {255, 0, 0, 255});
    WritePng(fixture.source / "bgra.png", 1, 1, red);
    WritePng(fixture.source / "half.png", 1, 1, red);
    WritePng(fixture.source / "float.png", 1, 1, red);
    WriteTextureMeta(fixture.source / "bgra.png", "assets/bgra.png",
                     R"({"format":"bgra8","generateMips":false})");
    WriteTextureMeta(fixture.source / "half.png", "assets/half.png",
                     R"({"format":"rgba16f","generateMips":false})");
    WriteTextureMeta(fixture.source / "float.png", "assets/float.png",
                     R"({"format":"rgba32f","generateMips":false})");

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto result = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 3);
    CHECK(result.Value().failed == 0);

    auto bgra = DecodeTextureOutput(fixture, fixture.source / "bgra.png");
    CHECK(bgra->format == assets::TextureFormat::BGRA8);
    CHECK(bgra->mips[0].row_pitch == 4);
    REQUIRE(bgra->mips[0].data.size() == 4);
    CHECK(std::to_integer<std::uint8_t>(bgra->mips[0].data[0]) == 0);
    CHECK(std::to_integer<std::uint8_t>(bgra->mips[0].data[1]) == 0);
    CHECK(std::to_integer<std::uint8_t>(bgra->mips[0].data[2]) == 255);
    CHECK(std::to_integer<std::uint8_t>(bgra->mips[0].data[3]) == 255);

    auto half = DecodeTextureOutput(fixture, fixture.source / "half.png");
    CHECK(half->format == assets::TextureFormat::RGBA16F);
    CHECK(half->mips[0].row_pitch == 8);
    REQUIRE(half->mips[0].data.size() == 8);
    CHECK(std::to_integer<std::uint8_t>(half->mips[0].data[0]) == 0);
    CHECK(std::to_integer<std::uint8_t>(half->mips[0].data[1]) == 0x3c);
    CHECK(std::to_integer<std::uint8_t>(half->mips[0].data[2]) == 0);
    CHECK(std::to_integer<std::uint8_t>(half->mips[0].data[3]) == 0);
    CHECK(std::to_integer<std::uint8_t>(half->mips[0].data[4]) == 0);
    CHECK(std::to_integer<std::uint8_t>(half->mips[0].data[5]) == 0);
    CHECK(std::to_integer<std::uint8_t>(half->mips[0].data[6]) == 0);
    CHECK(std::to_integer<std::uint8_t>(half->mips[0].data[7]) == 0x3c);

    auto floats = DecodeTextureOutput(fixture, fixture.source / "float.png");
    CHECK(floats->format == assets::TextureFormat::RGBA32F);
    CHECK(floats->mips[0].row_pitch == 16);
    REQUIRE(floats->mips[0].data.size() == 16);
    float r = 0.0F;
    float a = 0.0F;
    std::memcpy(&r, floats->mips[0].data.data(), sizeof(float));
    std::memcpy(&a, floats->mips[0].data.data() + 12, sizeof(float));
    CHECK(r == 1.0F);
    CHECK(a == 1.0F);
}

TEST_CASE("texture importer converts HDR to half floats and rejects block compression") {
    ImportFixture fixture;
    const std::array<std::uint8_t, 4> red = {128, 0, 0, 129};
    WriteHdr(fixture.source / "hdr16.hdr", 1, 1, red);
    WriteHdr(fixture.source / "hdrbc.hdr", 1, 1, red);
    WriteTextureMeta(fixture.source / "hdr16.hdr", "assets/hdr16.hdr",
                     R"({"format":"rgba16f","generateMips":false})");
    WriteTextureMeta(fixture.source / "hdrbc.hdr", "assets/hdrbc.hdr",
                     R"({"compression":"bc7","generateMips":false})");

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto result = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 1);
    CHECK(result.Value().failed == 1);

    auto hdr = DecodeTextureOutput(fixture, fixture.source / "hdr16.hdr");
    CHECK(hdr->format == assets::TextureFormat::RGBA16F);
    CHECK_FALSE(hdr->sRGB);
    CHECK(hdr->mips[0].row_pitch == 8);
    REQUIRE(hdr->mips[0].data.size() == 8);
    CHECK(std::to_integer<std::uint8_t>(hdr->mips[0].data[0]) == 0);
    CHECK(std::to_integer<std::uint8_t>(hdr->mips[0].data[1]) == 0x3c);
    CHECK(std::to_integer<std::uint8_t>(hdr->mips[0].data[7]) == 0x3c);
}

TEST_CASE("texture importer encodes block compressed formats") {
    ImportFixture fixture;
    const std::vector<std::uint8_t> red = SolidPixels(4, 4, {255, 0, 0, 255});
    for (const char* name : {"bc1.png", "bc3.png", "bc5.png", "bc7.png"}) {
        WritePng(fixture.source / name, 4, 4, red);
    }
    WriteTextureMeta(fixture.source / "bc1.png", "assets/bc1.png",
                     R"({"compression":"bc1","generateMips":false})");
    WriteTextureMeta(fixture.source / "bc3.png", "assets/bc3.png",
                     R"({"compression":"bc3","generateMips":false})");
    WriteTextureMeta(fixture.source / "bc5.png", "assets/bc5.png",
                     R"({"compression":"bc5","generateMips":false})");
    WriteTextureMeta(fixture.source / "bc7.png", "assets/bc7.png",
                     R"({"compression":"bc7","generateMips":false})");

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto result = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 4);
    CHECK(result.Value().failed == 0);

    auto bc1 = DecodeTextureOutput(fixture, fixture.source / "bc1.png");
    CHECK(bc1->format == assets::TextureFormat::BC1);
    CHECK(bc1->mips[0].row_pitch == 8);
    REQUIRE(bc1->mips[0].data.size() == 8);
    const auto bc1_pixel = DecodeBc1Pixel(bc1->mips[0].data, 0, 0);
    CHECK(bc1_pixel[0] >= 248);
    CHECK(bc1_pixel[1] <= 7);
    CHECK(bc1_pixel[2] <= 7);

    auto bc3 = DecodeTextureOutput(fixture, fixture.source / "bc3.png");
    CHECK(bc3->format == assets::TextureFormat::BC3);
    CHECK(bc3->mips[0].row_pitch == 16);
    REQUIRE(bc3->mips[0].data.size() == 16);
    CHECK(DecodeBc4Pixel(bc3->mips[0].data, 0, 0) == 255);
    const auto bc3_pixel = DecodeBc1Pixel(bc3->mips[0].data, 8, 0);
    CHECK(bc3_pixel[0] >= 248);
    CHECK(bc3_pixel[1] <= 7);

    auto bc5 = DecodeTextureOutput(fixture, fixture.source / "bc5.png");
    CHECK(bc5->format == assets::TextureFormat::BC5);
    CHECK(bc5->mips[0].row_pitch == 16);
    REQUIRE(bc5->mips[0].data.size() == 16);
    CHECK(DecodeBc4Pixel(bc5->mips[0].data, 0, 0) >= 248);
    CHECK(DecodeBc4Pixel(bc5->mips[0].data, 8, 0) <= 7);

    auto bc7 = DecodeTextureOutput(fixture, fixture.source / "bc7.png");
    CHECK(bc7->format == assets::TextureFormat::BC7);
    CHECK(bc7->mips[0].row_pitch == 16);
    REQUIRE(bc7->mips[0].data.size() == 16);
    CHECK(std::any_of(bc7->mips[0].data.begin(), bc7->mips[0].data.end(),
                      [](std::byte value) { return value != std::byte{0}; }));
}

TEST_CASE("texture importer auto selects BC1 or BC3 and compresses mip chains") {
    ImportFixture fixture;
    const std::vector<std::uint8_t> red = SolidPixels(8, 8, {255, 0, 0, 255});
    const std::vector<std::uint8_t> transparent = SolidPixels(4, 4, {255, 0, 0, 0});
    WritePng(fixture.source / "opaque.png", 8, 8, red);
    WritePng(fixture.source / "alpha.png", 4, 4, transparent);
    WriteTextureMeta(fixture.source / "opaque.png", "assets/opaque.png",
                     R"({"compression":"auto"})");
    WriteTextureMeta(fixture.source / "alpha.png", "assets/alpha.png", R"({"compression":"auto"})");

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto result = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 2);
    CHECK(result.Value().failed == 0);

    auto opaque = DecodeTextureOutput(fixture, fixture.source / "opaque.png");
    CHECK(opaque->format == assets::TextureFormat::BC1);
    REQUIRE(opaque->mips.size() == 4);
    CHECK(opaque->mips[0].row_pitch == 16);
    CHECK(opaque->mips[0].data.size() == 32);
    CHECK(opaque->mips[1].row_pitch == 8);
    CHECK(opaque->mips[1].data.size() == 8);
    CHECK(opaque->mips[3].row_pitch == 8);
    CHECK(opaque->mips[3].data.size() == 8);

    auto alpha = DecodeTextureOutput(fixture, fixture.source / "alpha.png");
    CHECK(alpha->format == assets::TextureFormat::BC3);
    REQUIRE(alpha->mips.size() == 3);
    CHECK(alpha->mips[0].row_pitch == 16);
    CHECK(alpha->mips[0].data.size() == 16);
    CHECK(DecodeBc4Pixel(alpha->mips[0].data, 0, 0) == 0);
}

TEST_CASE("texture importer rejects invalid encoding combinations") {
    ImportFixture fixture;
    const std::vector<std::uint8_t> red = SolidPixels(4, 4, {255, 0, 0, 255});
    WritePng(fixture.source / "conflict.png", 4, 4, red);
    WritePng(fixture.source / "unknown.png", 4, 4, red);
    WriteTextureMeta(fixture.source / "conflict.png", "assets/conflict.png",
                     R"({"format":"rgba16f","compression":"bc7"})");
    WriteTextureMeta(fixture.source / "unknown.png", "assets/unknown.png",
                     R"({"compression":"astc"})");

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto result = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 0);
    CHECK(result.Value().failed == 2);
}

TEST_CASE("import pipeline reimports after source changes") {
    ImportFixture fixture;
    const std::array<std::uint8_t, 16> pixels = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
    };
    WriteTga(fixture.source / "texture.tga", 2, 2, pixels);
    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    const auto options = fixture.Options();
    REQUIRE(import::RunImport(options, importers.Value()));

    auto changed = pixels;
    changed[0] = 0;
    WriteTga(fixture.source / "texture.tga", 2, 2, changed);
    auto result = import::RunImport(options, importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 1);
    CHECK(result.Value().skipped == 0);
}

TEST_CASE("import pipeline reuses identical texture content across sources") {
    ImportFixture fixture;
    const std::vector<std::uint8_t> pixels = SolidPixels(2, 2, {10, 20, 30, 255});
    WriteTga(fixture.source / "b.tga", 2, 2, pixels);

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto first = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(first);
    CHECK(first.Value().imported == 1);
    CHECK(first.Value().failed == 0);

    auto b_meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "b.tga.parcelmeta"));
    REQUIRE(b_meta);
    REQUIRE(b_meta.Value().assets.size() == 1);
    const AssetGuid canonical = b_meta.Value().assets[0].guid;
    const auto b_hash = core::Sha256File(ToUtf8(fixture.source / "b.tga"));
    REQUIRE(b_hash);
    CHECK(canonical ==
          import::TextureAssetGuidForContent(
              b_hash.Value().ToHex(), import::HashSettings(b_meta.Value().settings).ToHex()));

    WriteTga(fixture.source / "a.tga", 2, 2, pixels);
    auto second = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(second);
    CHECK(second.Value().imported == 1);
    CHECK(second.Value().skipped == 1);
    CHECK(second.Value().failed == 0);

    auto a_meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "a.tga.parcelmeta"));
    REQUIRE(a_meta);
    REQUIRE(a_meta.Value().assets.size() == 1);
    CHECK(a_meta.Value().assets[0].guid == canonical);
    b_meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "b.tga.parcelmeta"));
    REQUIRE(b_meta);
    REQUIRE(b_meta.Value().assets.size() == 1);
    CHECK(b_meta.Value().assets[0].guid == canonical);

    std::size_t cooked_textures = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(fixture.output)) {
        if (entry.path().extension() == ".prca") {
            ++cooked_textures;
        }
    }
    CHECK(cooked_textures == 1);
}

TEST_CASE("import pipeline keeps texture settings separate for identical content") {
    ImportFixture fixture;
    const std::vector<std::uint8_t> pixels = SolidPixels(2, 2, {10, 20, 30, 255});
    WriteTga(fixture.source / "rgba.tga", 2, 2, pixels);
    WriteTga(fixture.source / "bgra.tga", 2, 2, pixels);
    WriteTextureMeta(fixture.source / "rgba.tga", "assets/rgba.tga",
                     R"({"format":"rgba8","generateMips":false})");
    WriteTextureMeta(fixture.source / "bgra.tga", "assets/bgra.tga",
                     R"({"format":"bgra8","generateMips":false})");

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto result = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 2);
    CHECK(result.Value().failed == 0);

    auto rgba_meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "rgba.tga.parcelmeta"));
    auto bgra_meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "bgra.tga.parcelmeta"));
    REQUIRE(rgba_meta);
    REQUIRE(bgra_meta);
    REQUIRE(rgba_meta.Value().assets.size() == 1);
    REQUIRE(bgra_meta.Value().assets.size() == 1);
    CHECK(rgba_meta.Value().assets[0].guid != bgra_meta.Value().assets[0].guid);
}

TEST_CASE("texture assets split when aliased settings diverge") {
    ImportFixture fixture;
    const std::vector<std::uint8_t> pixels = SolidPixels(2, 2, {10, 20, 30, 255});
    WriteTga(fixture.source / "a.tga", 2, 2, pixels);
    WriteTga(fixture.source / "b.tga", 2, 2, pixels);

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto first = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(first);
    CHECK(first.Value().imported == 2);

    auto a_meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "a.tga.parcelmeta"));
    auto b_meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "b.tga.parcelmeta"));
    REQUIRE(a_meta);
    REQUIRE(b_meta);
    REQUIRE(a_meta.Value().assets.size() == 1);
    REQUIRE(b_meta.Value().assets.size() == 1);
    const AssetGuid shared = a_meta.Value().assets[0].guid;
    CHECK(b_meta.Value().assets[0].guid == shared);

    WriteTextureMeta(fixture.source / "a.tga", "assets/a.tga",
                     R"({"format":"bgra8","generateMips":false})");
    auto second = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(second);
    CHECK(second.Value().imported == 1);
    CHECK(second.Value().skipped == 1);

    a_meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "a.tga.parcelmeta"));
    b_meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "b.tga.parcelmeta"));
    REQUIRE(a_meta);
    REQUIRE(b_meta);
    REQUIRE(a_meta.Value().assets.size() == 1);
    REQUIRE(b_meta.Value().assets.size() == 1);
    CHECK(a_meta.Value().assets[0].guid != shared);
    CHECK(b_meta.Value().assets[0].guid == shared);
}

TEST_CASE("shader importer packs manifest binaries") {
    ImportFixture fixture;
    const auto shaders = fixture.source / "shaders";
    std::filesystem::create_directories(shaders / "build" / "dxbc");
    WriteTextFile(shaders / "build" / "dxbc" / "forward.vs.bin", "vertex");
    WriteTextFile(shaders / "build" / "dxbc" / "forward.fs.bin", "fragment");
    WriteTextFile(shaders / "manifest.json", R"json({
        "version": 1,
        "passes": {
            "dxbc": {
                "forward": {
                    "vertex": "build/dxbc/forward.vs.bin",
                    "fragments": ["build/dxbc/forward.fs.bin"]
                }
            }
        }
    })json");

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto options = fixture.Options();
    auto result = import::RunImport(options, importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 1);
    CHECK(result.Value().failed == 0);

    auto meta = import::LoadAssetMetaFile(ToUtf8(shaders / "manifest.json.parcelmeta"));
    REQUIRE(meta);
    REQUIRE(meta.Value().assets.size() == 1);
    TypeRegistry registry;
    REQUIRE(assets::RegisterStandardAssetSerializers(registry));
    auto object =
        DecodeOutput(fixture.output / (meta.Value().assets[0].guid.ToHex() + ".prca"), registry);
    auto shader = std::static_pointer_cast<assets::ShaderLibraryAsset>(object);
    REQUIRE(shader);
    REQUIRE(shader->backends.size() == 1);
    CHECK(shader->backends[0].backend == assets::ShaderBackend::Dxbc);
    REQUIRE(shader->backends[0].passes.size() == 2);
    CHECK(shader->backends[0].passes[0].data == Bytes("vertex"));
    CHECK(shader->backends[0].passes[1].data == Bytes("fragment"));
}

TEST_CASE("shader importer reports missing binaries") {
    ImportFixture fixture;
    const auto shaders = fixture.source / "shaders";
    std::filesystem::create_directories(shaders);
    WriteTextFile(shaders / "manifest.json", R"json({
        "version": 1,
        "passes": {"dxbc": {"forward": {"vertex": "missing.vs.bin", "fragment": "missing.fs.bin"}}}
    })json");
    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto options = fixture.Options();
    auto result = import::RunImport(options, importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 0);
    CHECK(result.Value().failed == 1);
    REQUIRE(result.Value().entries.size() == 1);
    CHECK(result.Value().entries[0].error.find("missing.vs.bin") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(shaders / "manifest.json.parcelmeta"));
}

TEST_CASE("import pipeline invokes shader build and forces shader reimport") {
    ImportFixture fixture;
    const auto tools = fixture.project / "tools";
    const auto bgfx_src = fixture.project / "bgfx";
    std::filesystem::create_directories(tools);
    std::filesystem::create_directories(bgfx_src);
    WriteTextFile(tools / "shaderc.exe", "fake shaderc");
    REQUIRE(std::filesystem::is_regular_file(PARCEL_FAKE_SHADER_BUILD));

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto options = fixture.Options();
    import::ShaderBuildOptions build;
    build.script = PARCEL_FAKE_SHADER_BUILD;
    build.shaderc = ToUtf8(tools / "shaderc.exe");
    build.bgfx_src = ToUtf8(bgfx_src);
    build.root = ToUtf8(fixture.source);
    build.jobs = 2;
    build.timeout_seconds = 0;
    options.shader_build = build;

    auto first = import::RunImport(options, importers.Value());
    REQUIRE(first);
    CHECK(first.Value().imported == 1);
    CHECK(first.Value().skipped == 0);
    CHECK(first.Value().failed == 0);

    const auto shaders = fixture.source / "shaders";
    REQUIRE(std::filesystem::is_regular_file(shaders / "manifest.json"));
    auto meta = import::LoadAssetMetaFile(ToUtf8(shaders / "manifest.json.parcelmeta"));
    REQUIRE(meta);
    REQUIRE(meta.Value().assets.size() == 1);
    TypeRegistry registry;
    REQUIRE(assets::RegisterStandardAssetSerializers(registry));
    auto object =
        DecodeOutput(fixture.output / (meta.Value().assets[0].guid.ToHex() + ".prca"), registry);
    auto shader = std::static_pointer_cast<assets::ShaderLibraryAsset>(object);
    REQUIRE(shader);
    REQUIRE(shader->backends.size() == 1);
    REQUIRE(shader->backends[0].passes.size() == 2);
    CHECK(shader->backends[0].passes[0].data == Bytes("fake vertex"));
    CHECK(shader->backends[0].passes[1].data == Bytes("fake fragment"));

    const auto arguments = ReadFile(fixture.project / "shader_build_args.txt");
    const std::string arguments_text(reinterpret_cast<const char*>(arguments.data()),
                                     arguments.size());
    CHECK(arguments_text.find("--no-progress\n") != std::string::npos);
    CHECK(arguments_text.find("--jobs\n2\n") != std::string::npos);
    CHECK(arguments_text.find("--timeout\n0\n") != std::string::npos);
    CHECK(arguments_text.find("--force\n") == std::string::npos);

    build.force = true;
    options.shader_build = build;
    auto second = import::RunImport(options, importers.Value());
    REQUIRE(second);
    CHECK(second.Value().imported == 1);
    CHECK(second.Value().skipped == 0);
    const auto forced_arguments = ReadFile(fixture.project / "shader_build_args.txt");
    const std::string forced_arguments_text(reinterpret_cast<const char*>(forced_arguments.data()),
                                            forced_arguments.size());
    CHECK(forced_arguments_text.find("--force\n") != std::string::npos);

    auto third = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(third);
    CHECK(third.Value().imported == 0);
    CHECK(third.Value().skipped == 1);
}

TEST_CASE("import pipeline aborts when shader build fails") {
    ImportFixture fixture;
    const auto tools = fixture.project / "tools";
    const auto bgfx_src = fixture.project / "bgfx";
    std::filesystem::create_directories(tools);
    std::filesystem::create_directories(bgfx_src);
    WriteTextFile(tools / "shaderc.exe", "fake shaderc");
    WriteTextFile(fixture.project / "shader_build_fail.txt", "fail");

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto options = fixture.Options();
    import::ShaderBuildOptions build;
    build.script = PARCEL_FAKE_SHADER_BUILD;
    build.shaderc = ToUtf8(tools / "shaderc.exe");
    build.bgfx_src = ToUtf8(bgfx_src);
    build.root = ToUtf8(fixture.source);
    build.timeout_seconds = 0;
    options.shader_build = build;

    auto result = import::RunImport(options, importers.Value());
    REQUIRE_FALSE(result);
    CHECK(result.GetError().code == core::ErrorCode::Io);
    CHECK(result.GetError().message.find("exit code 7") != std::string::npos);
    CHECK(result.GetError().message.find("fake shader build failure") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(fixture.source / "shaders" / "manifest.json"));
}

TEST_CASE("import pipeline skips shader build in dry-run") {
    ImportFixture fixture;
    const auto tools = fixture.project / "tools";
    const auto bgfx_src = fixture.project / "bgfx";
    std::filesystem::create_directories(tools);
    std::filesystem::create_directories(bgfx_src);
    WriteTextFile(tools / "shaderc.exe", "fake shaderc");
    WriteTextFile(fixture.project / "shader_build_fail.txt", "fail");

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto options = fixture.Options();
    options.dry_run = true;
    import::ShaderBuildOptions build;
    build.script = PARCEL_FAKE_SHADER_BUILD;
    build.shaderc = ToUtf8(tools / "shaderc.exe");
    build.bgfx_src = ToUtf8(bgfx_src);
    build.root = ToUtf8(fixture.source);
    options.shader_build = build;

    auto result = import::RunImport(options, importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 0);
    CHECK(result.Value().failed == 0);
    CHECK_FALSE(std::filesystem::exists(fixture.source / "shaders" / "manifest.json"));
}

TEST_CASE("import pipeline cooks IES profiles and reports invalid files") {
    ImportFixture fixture;
    constexpr std::string_view kValidIes = R"(IESNA:LM-63-2002
TILT=NONE
1 1000 1 3 4 1 2 0 0 0
0 45 90
0 90 180 270
1 1 1 1
0.5 0.5 0.5 0.5
0.25 0.25 0.25 0.25
)";
    WriteTextFile(fixture.source / "profile.ies", kValidIes);
    WriteTextFile(fixture.source / "broken.ies", "TILT=INCLUDE\n1 1 1 2 1 1 2 0 0 0\n");

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto result = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 1);
    CHECK(result.Value().failed == 1);

    auto meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "profile.ies.parcelmeta"));
    REQUIRE(meta);
    REQUIRE(meta.Value().assets.size() == 1);
    CHECK(meta.Value().importer == "ies");
    CHECK(meta.Value().assets[0].type == "IesProfileAsset");
    CHECK(meta.Value().assets[0].guid == import::DefaultIesAssetGuid("assets/profile.ies"));

    TypeRegistry registry;
    REQUIRE(assets::RegisterStandardAssetSerializers(registry));
    const auto path = fixture.output / (meta.Value().assets[0].guid.ToHex() + ".prca");
    auto object = DecodeOutput(path, registry);
    auto profile = std::static_pointer_cast<assets::IesProfileAsset>(object);
    REQUIRE(profile);
    CHECK(profile->name == "profile.ies");
    REQUIRE(profile->vertical_angles.size() == 3);
    REQUIRE(profile->horizontal_angles.size() == 4);
    REQUIRE(profile->values.size() == 12);
    CHECK(profile->vertical_angles[1] == Catch::Approx(45.0f));
    CHECK(profile->horizontal_angles[3] == Catch::Approx(270.0f));
    CHECK(profile->values[4] == Catch::Approx(0.5f));
    CHECK(profile->values[11] == Catch::Approx(0.25f));
}

TEST_CASE("import content hashes ignore names and preserve material assignments") {
    assets::MaterialAsset material;
    material.name = "First";
    material.base_color_factor = {0.8f, 0.7f, 0.6f, 1.0f};
    material.roughness = 0.4f;
    auto first_material = import::HashMaterialContent(material);
    REQUIRE(first_material);

    assets::MaterialAsset renamed = material;
    renamed.name = "Second";
    auto second_material = import::HashMaterialContent(renamed);
    REQUIRE(second_material);
    CHECK(first_material.Value() == second_material.Value());

    renamed.roughness = 0.5f;
    auto changed_material = import::HashMaterialContent(renamed);
    REQUIRE(changed_material);
    CHECK(changed_material.Value() != first_material.Value());

    assets::MeshAsset mesh;
    mesh.name = "First";
    mesh.vertices.push_back(assets::MeshVertex{});
    mesh.indices = {0};
    mesh.bounds.center = {1.0f, 2.0f, 3.0f};
    mesh.bounds.extents = {1.0f, 1.0f, 1.0f};
    const AssetGuid material_guid = import::MakeSourceGuid("assets/material.parcel");
    const std::vector<AssetGuid> materials{material_guid};
    auto first_mesh = import::HashMeshContent(mesh, materials);
    REQUIRE(first_mesh);

    assets::MeshAsset renamed_mesh = mesh;
    renamed_mesh.name = "Second";
    auto second_mesh = import::HashMeshContent(renamed_mesh, materials);
    REQUIRE(second_mesh);
    CHECK(first_mesh.Value() == second_mesh.Value());

    renamed_mesh.vertices[0].position.x = 1.0f;
    auto changed_mesh = import::HashMeshContent(renamed_mesh, materials);
    REQUIRE(changed_mesh);
    CHECK(changed_mesh.Value() != first_mesh.Value());

    const std::vector<AssetGuid> other_materials{import::MakeSourceGuid("assets/other.parcel")};
    auto other_material_mesh = import::HashMeshContent(mesh, other_materials);
    REQUIRE(other_material_mesh);
    CHECK(other_material_mesh.Value() != first_mesh.Value());
}

#if defined(PARCEL_ASSIMP_ANIMATION_MODEL)
TEST_CASE("fbx importer cooks node animations and player component") {
    ImportFixture fixture;
    const std::filesystem::path model{PARCEL_ASSIMP_ANIMATION_MODEL};
    REQUIRE(std::filesystem::exists(model));
    std::filesystem::copy_file(model, fixture.source / "animated.fbx");

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto result = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 1);
    CHECK(result.Value().failed == 0);

    auto meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "animated.fbx.parcelmeta"));
    REQUIRE(meta);
    CHECK(meta.Value().importer == "fbx");
    AssetGuid scene_guid;
    std::vector<AssetGuid> animation_guids;
    std::vector<AssetGuid> mesh_guids;
    for (const import::MetaAsset& asset : meta.Value().assets) {
        if (asset.type == "SceneAsset") {
            scene_guid = asset.guid;
        } else if (asset.type == "AnimationClipAsset") {
            animation_guids.push_back(asset.guid);
        } else if (asset.type == "MeshAsset") {
            mesh_guids.push_back(asset.guid);
        }
    }
    CHECK_FALSE(scene_guid.IsZero());
    CHECK_FALSE(animation_guids.empty());
    REQUIRE(mesh_guids.size() == 1);

    TypeRegistry registry;
    REQUIRE(assets::RegisterStandardAssetSerializers(registry));
    auto scene_object = DecodeOutput(fixture.output / (scene_guid.ToHex() + ".prca"), registry);
    auto scene = std::static_pointer_cast<assets::SceneAsset>(scene_object);
    REQUIRE(scene);

    auto mesh_object =
        DecodeOutput(fixture.output / (mesh_guids.front().ToHex() + ".prca"), registry);
    auto mesh = std::static_pointer_cast<assets::MeshAsset>(mesh_object);
    REQUIRE(mesh);
    REQUIRE_FALSE(mesh->skin_vertices.empty());
    CHECK(mesh->skin_vertices.size() == mesh->vertices.size());
    for (const assets::SkinVertex& skin : mesh->skin_vertices) {
        float total = 0.0f;
        for (float weight : skin.weights) {
            total += weight;
        }
        CHECK(total == Catch::Approx(1.0f));
    }

    const auto* skin_chunk = scene->scene.FindComponent(parcel::scene::kSkinComponentType);
    REQUIRE(skin_chunk != nullptr);
    REQUIRE(skin_chunk->payloads.size() == 1);
    auto skin = parcel::scene::ComponentCodec<parcel::scene::SkinComponent>::Decode(
        skin_chunk->payloads.front());
    REQUIRE(skin);
    CHECK(skin.Value().joints.size() == 14);
    CHECK(skin.Value().joints.size() == skin.Value().inverse_bind_matrices.size());
    for (const parcel::scene::SceneEntityId joint : skin.Value().joints) {
        CHECK(scene->scene.FindEntity(joint) != nullptr);
    }

    const auto* chunk = scene->scene.FindComponent(parcel::scene::kAnimationPlayerComponentType);
    REQUIRE(chunk != nullptr);
    REQUIRE(chunk->payloads.size() == 1);
    auto player = parcel::scene::ComponentCodec<parcel::scene::AnimationPlayerComponent>::Decode(
        chunk->payloads.front());
    REQUIRE(player);
    CHECK(player.Value().clips.size() == animation_guids.size());

    std::size_t checked_channels = 0;
    for (const AssetGuid& guid : player.Value().clips) {
        auto clip_object = DecodeOutput(fixture.output / (guid.ToHex() + ".prca"), registry);
        auto clip = std::static_pointer_cast<assets::AnimationClipAsset>(clip_object);
        REQUIRE(clip);
        REQUIRE_FALSE(clip->channels.empty());
        for (const assets::AnimationChannel& channel : clip->channels) {
            CHECK(scene->scene.FindEntity(channel.target) != nullptr);
            ++checked_channels;
        }
    }
    CHECK(checked_channels > 0);
}
#endif

#if defined(PARCEL_ASSIMP_MORPH_MODEL)
TEST_CASE("fbx importer imports morph targets and morph animation channels") {
    ImportFixture fixture;
    const std::filesystem::path model{PARCEL_ASSIMP_MORPH_MODEL};
    REQUIRE(std::filesystem::exists(model));
    std::filesystem::copy_file(model, fixture.source / "morph.fbx");

    auto importers = import::CreateStandardImporterRegistry();
    REQUIRE(importers);
    auto result = import::RunImport(fixture.Options(), importers.Value());
    REQUIRE(result);
    CHECK(result.Value().imported == 1);
    CHECK(result.Value().failed == 0);

    auto meta = import::LoadAssetMetaFile(ToUtf8(fixture.source / "morph.fbx.parcelmeta"));
    REQUIRE(meta);
    AssetGuid scene_guid;
    std::vector<AssetGuid> animation_guids;
    std::unordered_map<std::string, const import::MetaAsset*> assets_by_guid;
    for (const import::MetaAsset& asset : meta.Value().assets) {
        assets_by_guid.emplace(asset.guid.ToHex(), &asset);
        if (asset.type == "SceneAsset") {
            scene_guid = asset.guid;
        } else if (asset.type == "AnimationClipAsset") {
            animation_guids.push_back(asset.guid);
        }
    }
    CHECK_FALSE(scene_guid.IsZero());
    REQUIRE_FALSE(animation_guids.empty());

    TypeRegistry registry;
    REQUIRE(assets::RegisterStandardAssetSerializers(registry));
    auto scene_object = DecodeOutput(fixture.output / (scene_guid.ToHex() + ".prca"), registry);
    auto scene = std::static_pointer_cast<assets::SceneAsset>(scene_object);
    REQUIRE(scene);

    std::unordered_map<std::uint64_t, AssetGuid> entity_meshes;
    const auto* renderer_chunk =
        scene->scene.FindComponent(parcel::scene::kMeshRendererComponentType);
    REQUIRE(renderer_chunk != nullptr);
    REQUIRE(renderer_chunk->owners.size() == renderer_chunk->payloads.size());
    for (std::size_t index = 0; index < renderer_chunk->payloads.size(); ++index) {
        auto renderer = parcel::scene::ComponentCodec<parcel::scene::MeshRendererComponent>::Decode(
            renderer_chunk->payloads[index]);
        REQUIRE(renderer);
        entity_meshes.emplace(renderer_chunk->owners[index].value, renderer.Value().mesh);
    }

    bool found_morph_mesh = false;
    for (const auto& entry : assets_by_guid) {
        const import::MetaAsset* asset = entry.second;
        if (asset->type != "MeshAsset") {
            continue;
        }
        auto mesh_object = DecodeOutput(fixture.output / (asset->guid.ToHex() + ".prca"), registry);
        auto mesh = std::static_pointer_cast<assets::MeshAsset>(mesh_object);
        REQUIRE(mesh);
        if (mesh->morph_targets.empty()) {
            continue;
        }
        found_morph_mesh = true;
        CHECK(mesh->morph_weights.size() == mesh->morph_targets.size());
        for (const assets::MorphTarget& target : mesh->morph_targets) {
            CHECK_FALSE(target.name.empty());
            REQUIRE(target.position_deltas.size() == mesh->vertices.size());
            CHECK((target.has_normals ? target.normal_deltas.size() == mesh->vertices.size()
                                      : target.normal_deltas.empty()));
            CHECK((target.has_tangents ? target.tangent_deltas.size() == mesh->vertices.size()
                                       : target.tangent_deltas.empty()));
            bool non_zero_delta = false;
            for (const Vec3& delta : target.position_deltas) {
                non_zero_delta =
                    non_zero_delta || delta.x != 0.0f || delta.y != 0.0f || delta.z != 0.0f;
            }
            CHECK(non_zero_delta);
        }
        for (float weight : mesh->morph_weights) {
            CHECK(weight == Catch::Approx(0.0f));
        }
    }
    CHECK(found_morph_mesh);

    std::size_t weight_channels = 0;
    bool found_channel_on_morph_mesh = false;
    for (const AssetGuid& guid : animation_guids) {
        auto clip_object = DecodeOutput(fixture.output / (guid.ToHex() + ".prca"), registry);
        auto clip = std::static_pointer_cast<assets::AnimationClipAsset>(clip_object);
        REQUIRE(clip);
        REQUIRE(assets::ValidateAnimationClip(*clip));
        for (const assets::AnimationChannel& channel : clip->channels) {
            if (channel.path != assets::AnimationPath::Weights) {
                continue;
            }
            ++weight_channels;
            CHECK(scene->scene.FindEntity(channel.target) != nullptr);
            const auto entity_mesh = entity_meshes.find(channel.target.value);
            REQUIRE(entity_mesh != entity_meshes.end());
            auto mesh_object =
                DecodeOutput(fixture.output / (entity_mesh->second.ToHex() + ".prca"), registry);
            auto mesh = std::static_pointer_cast<assets::MeshAsset>(mesh_object);
            REQUIRE(mesh);
            CHECK_FALSE(mesh->morph_targets.empty());
            CHECK(channel.target_index < mesh->morph_targets.size());
            CHECK(channel.times.size() >= 2);
            CHECK(channel.times.size() == channel.values.size());
            found_channel_on_morph_mesh = true;
        }
    }
    CHECK(weight_channels > 0);
    CHECK(found_channel_on_morph_mesh);
}
#endif
