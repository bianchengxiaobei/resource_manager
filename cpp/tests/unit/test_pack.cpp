#include <catch_amalgamated.hpp>

#include "parcel/assets/asset_codec.hpp"
#include "parcel/assets/json.hpp"
#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/raw_file_asset.hpp"
#include "parcel/assets/serializers.hpp"
#include "parcel/assets/shader_library_asset.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/guid.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/pack/pack.hpp"
#include "parcel/serialize/builtin_catalog.hpp"
#include "parcel/serialize/bundle.hpp"
#include "parcel/serialize/manifest.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace parcel;
using parcel::core::AssetGuid;
using parcel::core::Hash256;
using parcel::test::TempDir;
using parcel::test::ToUtf8;

namespace {

AssetGuid GuidFrom(std::string_view name) {
    const Hash256 hash = core::Sha256("parcel.pack.test:" + std::string(name));
    std::array<std::byte, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(hash.bytes[i]);
    }
    return core::Guid128::FromBytes(std::span<const std::byte, 16>(bytes));
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
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    REQUIRE(file);
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    WriteFile(path, std::as_bytes(std::span<const char>(text.data(), text.size())));
}

AssetGuid WriteAsset(const std::filesystem::path& root, std::string_view folder,
                     std::string_view name, const assets::AssetObject& object,
                     const assets::TypeRegistry& registry) {
    const AssetGuid guid = GuidFrom(name);
    auto bytes = assets::EncodeAssetBytes(object, registry);
    REQUIRE(bytes);
    const std::filesystem::path directory = root / std::u8string(folder.begin(), folder.end());
    WriteFile(directory / (guid.ToHex() + ".prca"), bytes.Value());
    return guid;
}

struct PackFixture {
    TempDir directory{"pack"};
    std::filesystem::path project = directory.Path() / "project";
    std::filesystem::path cooked = project / "cooked";
    std::filesystem::path output = directory.Path() / "build";
    assets::TypeRegistry registry;
    AssetGuid texture_guid;
    AssetGuid material_a_guid;
    AssetGuid material_b_guid;
    AssetGuid raw_guid;

    PackFixture() {
        std::filesystem::create_directories(cooked);
        REQUIRE(assets::RegisterStandardAssetSerializers(registry));
        CreateDefaultAssets();
    }

    void CreateDefaultAssets() {
        assets::TextureAsset texture;
        texture.format = assets::TextureFormat::RGBA8;
        texture.sRGB = true;
        texture.mip_count = 1;
        assets::TextureMip mip;
        mip.width = 2;
        mip.height = 2;
        mip.row_pitch = 8;
        mip.data.assign(16, std::byte{0x7f});
        texture.mips.push_back(std::move(mip));
        texture_guid = WriteAsset(cooked, "textures", "texture", texture, registry);

        assets::MaterialAsset material_a;
        material_a.name = "material-a";
        material_a.base_color_texture.texture = texture_guid;
        material_a_guid = WriteAsset(cooked, "a", "material-a", material_a, registry);

        assets::MaterialAsset material_b;
        material_b.name = "material-b";
        material_b.base_color_texture.texture = texture_guid;
        material_b_guid = WriteAsset(cooked, "b", "material-b", material_b, registry);

        assets::RawFileAsset raw;
        raw.name = "payload";
        raw.data = {std::byte{1}, std::byte{2}, std::byte{3}};
        raw_guid = WriteAsset(cooked, "raw", "payload", raw, registry);
    }

    parcel::pack::CollectorConfig Collector(std::string name, parcel::pack::CollectorType type,
                                            std::string path) const {
        parcel::pack::CollectorConfig collector;
        collector.name = std::move(name);
        collector.type = type;
        collector.path = std::move(path);
        collector.address_rule = parcel::pack::AddressRule::AssetPath;
        return collector;
    }

    parcel::pack::PackConfig Config() const {
        parcel::pack::PackConfig config;
        config.project_root = ToUtf8(project);
        config.cooked_root = ToUtf8(cooked);
        config.output_root = ToUtf8(output);
        config.package_name = "game";
        config.package_version = "1.0.0";
        config.target = "windows";
        config.jobs = 2;
        config.collectors.push_back(Collector("main-a", parcel::pack::CollectorType::Main, "a"));
        config.collectors.push_back(Collector("main-b", parcel::pack::CollectorType::Main, "b"));
        config.collectors.push_back(
            Collector("textures", parcel::pack::CollectorType::Depend, "textures"));
        auto raw_collector = Collector("raw", parcel::pack::CollectorType::Main, "raw");
        raw_collector.bundle = "raw";
        config.collectors.push_back(std::move(raw_collector));
        return config;
    }
};

std::string OutputFile(const parcel::pack::PackSummary& summary, std::string_view name) {
    return summary.output_directory + "/" + std::string(name);
}

} // namespace

TEST_CASE("pack config parses collector rules") {
    TempDir directory{"pack_config"};
    const auto path = directory.Path() / "pack.json";
    WriteText(path, R"({
        "package": "game",
        "version": "2.0.0",
        "target": "windows",
        "source": "assets",
        "cooked": "cooked",
        "output": "build",
        "compression": "zstd",
        "blockSize": 131072,
        "verifyOutput": true,
        "writeBuiltinCatalog": false,
        "enableAddressable": false,
        "useSourcePath": false,
        "forceSubAssetGrouping": false,
        "collectors": [
            {
                "name": "main",
                "type": "main",
                "path": "assets",
                "addressRule": "prefix",
                "addressPrefix": "ui",
                "include": ["**/*.prca", "a/*.prca"],
                "exclude": ["raw/**"],
                "archive": true,
                "tags": ["base", "ui"],
                "bundle": "ui",
                "group": "ui-group",
                "packRule": "PackDirectory",
                "filterRule": "CollectMaterial"
            },
            {
                "name": "deps",
                "type": "depend",
                "files": ["shader.bin"],
                "addressRule": "address",
                "address": "fixed/address"
            }
        ]
    })");

    auto loaded = parcel::pack::LoadPackConfig(ToUtf8(path));
    REQUIRE(loaded);
    const auto& config = loaded.Value();
    REQUIRE(config.package_name == "game");
    REQUIRE(config.package_version == "2.0.0");
    REQUIRE(config.source_root == "assets");
    REQUIRE_FALSE(config.use_source_path);
    REQUIRE_FALSE(config.force_sub_asset_grouping);
    REQUIRE(config.compression == serialize::CompressionCodec::Zstd);
    REQUIRE(config.block_size == 131072);
    REQUIRE(config.verify_output_hashes);
    REQUIRE_FALSE(config.write_builtin_catalog);
    REQUIRE_FALSE(config.enable_addressable);
    REQUIRE(config.collectors.size() == 2);
    REQUIRE(config.collectors[0].address_rule == parcel::pack::AddressRule::Prefix);
    REQUIRE(config.collectors[0].address_prefix == "ui");
    REQUIRE(config.collectors[0].include_patterns ==
            std::vector<std::string>{"**/*.prca", "a/*.prca"});
    REQUIRE(config.collectors[0].exclude_patterns == std::vector<std::string>{"raw/**"});
    REQUIRE(config.collectors[0].tags == std::vector<std::string>{"base", "ui"});
    REQUIRE(config.collectors[0].bundle == "ui");
    REQUIRE(config.collectors[0].group == "ui-group");
    REQUIRE(config.collectors[0].pack_rule == parcel::pack::PackRule::PackDirectory);
    REQUIRE(config.collectors[0].filter_rule == parcel::pack::FilterRule::CollectMaterial);
    REQUIRE(config.collectors[0].archive);
    REQUIRE(config.collectors[1].type == parcel::pack::CollectorType::Depend);
    REQUIRE(config.collectors[1].address_rule == parcel::pack::AddressRule::Address);
    REQUIRE(config.collectors[1].address == "fixed/address");
}

TEST_CASE("pack config parses TOML collector rules") {
    TempDir directory{"pack_config_toml"};
    const auto path = directory.Path() / "pack.toml";
    WriteText(path, R"(# TOML pack config
package = "game"
version = "3.0.0"
target = "linux"
source = "assets"
cooked = "cooked"
output = "build"
compression = "zstd"
blockSize = 131072
jobs = 4
verifyOutput = true
writeBuiltinCatalog = false
autoCollectShaders = false
enableShareBundle = false
singleReferencedPackAlone = true

[[collectors]]
name = "main"
type = "main"
path = "assets"
include = [
  "**/*.prca", # all cooked assets
  "[ab]/*.prca",
]
exclude = ["raw/**"]
archive = true
recursive = false
addressRule = "prefix"
addressPrefix = "ui"
tags = ["base", "ui"]
bundle = 'ui'
group = "ui-group"
packRule = "PackCollector"
filterRule = "CollectTexture"

[[collectors]]
name = "deps"
type = "depend"
files = ["shader.bin"]
include = []
share = false
)");

    auto loaded = parcel::pack::LoadPackConfig(ToUtf8(path));
    REQUIRE(loaded);
    const auto& config = loaded.Value();
    REQUIRE(config.package_name == "game");
    REQUIRE(config.package_version == "3.0.0");
    REQUIRE(config.target == "linux");
    REQUIRE(config.source_root == "assets");
    REQUIRE(config.compression == serialize::CompressionCodec::Zstd);
    REQUIRE(config.block_size == 131072);
    REQUIRE(config.jobs == 4);
    REQUIRE(config.verify_output_hashes);
    REQUIRE_FALSE(config.write_builtin_catalog);
    REQUIRE_FALSE(config.auto_collect_shaders);
    REQUIRE_FALSE(config.enable_share_bundle);
    REQUIRE(config.single_referenced_pack_alone);
    REQUIRE(config.collectors.size() == 2);
    REQUIRE(config.collectors[0].include_patterns ==
            std::vector<std::string>{"**/*.prca", "[ab]/*.prca"});
    REQUIRE(config.collectors[0].exclude_patterns == std::vector<std::string>{"raw/**"});
    REQUIRE_FALSE(config.collectors[0].recursive);
    REQUIRE(config.collectors[0].address_prefix == "ui");
    REQUIRE(config.collectors[0].tags == std::vector<std::string>{"base", "ui"});
    REQUIRE(config.collectors[0].bundle == "ui");
    REQUIRE(config.collectors[0].group == "ui-group");
    REQUIRE(config.collectors[0].pack_rule == parcel::pack::PackRule::PackCollector);
    REQUIRE(config.collectors[0].filter_rule == parcel::pack::FilterRule::CollectTexture);
    REQUIRE(config.collectors[0].archive);
    REQUIRE(config.collectors[1].type == parcel::pack::CollectorType::Depend);
    REQUIRE(config.collectors[1].files == std::vector<std::string>{"shader.bin"});
    REQUIRE(config.collectors[1].include_patterns.empty());
    REQUIRE_FALSE(config.collectors[1].share);
}

TEST_CASE("pack config rejects malformed TOML") {
    TempDir directory{"pack_config_toml_bad"};
    const auto path = directory.Path() / "pack.toml";
    const std::array<std::string_view, 5> invalid = {
        "package = \"game\"\n[[collectors]\nname = \"main\"\n",
        "package = \"game\"\nname = \"a\"\nname = \"b\"\n",
        "package = \"game\"\nunsupported = { value = 1 }\n",
        "package = \"game\"\n[[other]]\nname = \"main\"\n",
        "package = \"game\"\n[[collectors]]\ninclude = [\"a\",\n",
    };
    for (const std::string_view text : invalid) {
        WriteText(path, text);
        auto loaded = parcel::pack::LoadPackConfig(ToUtf8(path));
        REQUIRE_FALSE(loaded);
        REQUIRE(loaded.GetError().code == core::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("pack config rejects unknown pack and filter rules") {
    TempDir directory{"pack_config_rules"};
    const auto path = directory.Path() / "pack.json";
    SECTION("unknown pack rule") {
        WriteText(path, R"({
            "package": "game",
            "version": "1.0.0",
            "cooked": "cooked",
            "output": "build",
            "collectors": [{"packRule": "PackBogus"}]
        })");
        auto loaded = parcel::pack::LoadPackConfig(ToUtf8(path));
        REQUIRE_FALSE(loaded);
        REQUIRE(loaded.GetError().code == core::ErrorCode::InvalidArgument);
    }
    SECTION("unknown filter rule") {
        WriteText(path, R"({
            "package": "game",
            "version": "1.0.0",
            "cooked": "cooked",
            "output": "build",
            "collectors": [{"filterRule": "CollectBogus"}]
        })");
        auto loaded = parcel::pack::LoadPackConfig(ToUtf8(path));
        REQUIRE_FALSE(loaded);
        REQUIRE(loaded.GetError().code == core::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("pack collectors expand include and exclude globs") {
    PackFixture fixture;
    auto config = fixture.Config();
    config.collectors.clear();
    auto collector = fixture.Collector("glob", parcel::pack::CollectorType::Main, "");
    collector.include_patterns = {"**/*.prca"};
    collector.exclude_patterns = {"raw/**"};
    config.collectors.push_back(std::move(collector));

    auto packed = parcel::pack::RunPack(config);
    REQUIRE(packed);
    const auto& report = packed.Value().report;
    REQUIRE(report.errors.empty());
    REQUIRE(report.asset_count == 3);
    REQUIRE(report.main_asset_count == 3);
    for (const auto& asset : report.assets) {
        REQUIRE(asset.guid != fixture.raw_guid);
    }
}

TEST_CASE("pack collector globs support character classes") {
    PackFixture fixture;
    auto config = fixture.Config();
    config.collectors.clear();
    auto main_collector = fixture.Collector("main", parcel::pack::CollectorType::Main, "");
    main_collector.include_patterns = {"[ab]/*.prca"};
    config.collectors.push_back(std::move(main_collector));
    auto texture_collector =
        fixture.Collector("textures", parcel::pack::CollectorType::Depend, "textures");
    texture_collector.include_patterns = {"*.prca"};
    config.collectors.push_back(std::move(texture_collector));

    auto packed = parcel::pack::RunPack(config);
    REQUIRE(packed);
    REQUIRE(packed.Value().report.asset_count == 3);
    REQUIRE(packed.Value().report.main_asset_count == 2);
}

TEST_CASE("pack rejects collector globs that escape the cooked root") {
    PackFixture fixture;
    auto config = fixture.Config();
    config.collectors.clear();
    auto collector = fixture.Collector("glob", parcel::pack::CollectorType::Main, "");
    collector.include_patterns = {"../*.prca"};
    config.collectors.push_back(std::move(collector));

    auto packed = parcel::pack::RunPack(config);
    REQUIRE_FALSE(packed);
    REQUIRE(packed.GetError().code == core::ErrorCode::InvalidArgument);
}

TEST_CASE("pack groups archive raw files into one bundle") {
    PackFixture fixture;
    assets::RawFileAsset second_raw;
    second_raw.name = "payload-2";
    second_raw.data = {std::byte{4}, std::byte{5}};
    const AssetGuid second_guid =
        WriteAsset(fixture.cooked, "raw", "payload-2", second_raw, fixture.registry);

    auto config = fixture.Config();
    config.verify_output_hashes = true;
    for (auto& collector : config.collectors) {
        if (collector.name == "raw") {
            collector.archive = true;
        }
    }

    auto packed = parcel::pack::RunPack(config);
    REQUIRE(packed);
    const auto& summary = packed.Value();
    REQUIRE(summary.report.errors.empty());
    REQUIRE(summary.report.asset_count == 5);
    REQUIRE(summary.report.main_asset_count == 4);
    REQUIRE(summary.report.bundle_count == 4);
    REQUIRE(summary.report.output_verified);

    auto manifest = serialize::LoadManifestFile(OutputFile(summary, "manifest.bytes"), true);
    REQUIRE(manifest);
    const auto* raw_view = manifest.Value().FindAssetByGuid(fixture.raw_guid);
    const auto* second_view = manifest.Value().FindAssetByGuid(second_guid);
    REQUIRE(raw_view != nullptr);
    REQUIRE(second_view != nullptr);
    REQUIRE((raw_view->flags & serialize::kManifestAssetFlagArchiveRaw) != 0);
    REQUIRE((second_view->flags & serialize::kManifestAssetFlagArchiveRaw) != 0);

    const auto* bundle_view = manifest.Value().FindBundleByName("raw");
    REQUIRE(bundle_view != nullptr);
    const auto bytes = ReadFile(std::filesystem::path(summary.output_directory) / "bundles" /
                                std::string(bundle_view->file_name));
    auto reader = serialize::BundleReader::Open(bytes, true);
    REQUIRE(reader);
    REQUIRE(reader.Value().ReadAll(true));
    REQUIRE(reader.Value().Toc().payloads.empty());
    REQUIRE(reader.Value().Toc().raw_files.size() == 2);

    std::vector<std::string> names;
    for (const auto& raw : reader.Value().Toc().raw_files) {
        names.push_back(raw.name);
        auto data = reader.Value().ReadLogical(raw.logical_offset, raw.size);
        REQUIRE(data);
        if (raw.name == raw_view->asset_path) {
            REQUIRE(data.Value() ==
                    std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}});
        } else {
            REQUIRE(raw.name == second_view->asset_path);
            REQUIRE(data.Value() == std::vector<std::byte>{std::byte{4}, std::byte{5}});
        }
    }
    REQUIRE(std::find(names.begin(), names.end(), raw_view->asset_path) != names.end());
    REQUIRE(std::find(names.begin(), names.end(), second_view->asset_path) != names.end());
}

TEST_CASE("pack collector filter rule selects cooked asset types") {
    PackFixture fixture;
    auto config = fixture.Config();
    config.collectors.clear();
    auto collector =
        fixture.Collector("textures-only", parcel::pack::CollectorType::Main, "textures");
    collector.filter_rule = parcel::pack::FilterRule::CollectTexture;
    config.collectors.push_back(std::move(collector));

    auto packed = parcel::pack::RunPack(config);
    REQUIRE(packed);
    REQUIRE(packed.Value().report.asset_count == 1);
    REQUIRE(packed.Value().report.bundle_count == 1);

    auto manifest = serialize::LoadManifestFile(OutputFile(packed.Value(), "manifest.bytes"), true);
    REQUIRE(manifest);
    REQUIRE(manifest.Value().Assets().size() == 1);
    REQUIRE(manifest.Value().Assets().front().type_name == "TextureAsset");
    REQUIRE(manifest.Value().FindAssetByGuid(fixture.texture_guid) != nullptr);
    REQUIRE(manifest.Value().FindAssetByGuid(fixture.material_a_guid) == nullptr);
}

TEST_CASE("pack filters fall back to the next matching collector") {
    PackFixture fixture;
    auto config = fixture.Config();
    config.collectors.clear();
    auto scene_only =
        fixture.Collector("scene-only", parcel::pack::CollectorType::Main, "textures");
    scene_only.filter_rule = parcel::pack::FilterRule::CollectScene;
    config.collectors.push_back(std::move(scene_only));
    auto texture_only =
        fixture.Collector("texture-only", parcel::pack::CollectorType::Main, "textures");
    texture_only.filter_rule = parcel::pack::FilterRule::CollectTexture;
    config.collectors.push_back(std::move(texture_only));

    auto packed = parcel::pack::RunPack(config);
    REQUIRE(packed);
    REQUIRE(packed.Value().report.asset_count == 1);
    REQUIRE(packed.Value().report.warnings.empty());

    auto manifest = serialize::LoadManifestFile(OutputFile(packed.Value(), "manifest.bytes"), true);
    REQUIRE(manifest);
    REQUIRE(manifest.Value().FindAssetByGuid(fixture.texture_guid) != nullptr);
}

TEST_CASE("pack rules control material bundle grouping") {
    PackFixture fixture;
    assets::MaterialAsset second_material;
    second_material.name = "material-a2";
    second_material.base_color_texture.texture = fixture.texture_guid;
    const AssetGuid second_material_guid =
        WriteAsset(fixture.cooked, "a", "material-a2", second_material, fixture.registry);

    SECTION("PackSeparately creates one bundle per material") {
        auto config = fixture.Config();
        config.collectors.clear();
        auto collector = fixture.Collector("materials", parcel::pack::CollectorType::Main, "a");
        collector.filter_rule = parcel::pack::FilterRule::CollectMaterial;
        collector.pack_rule = parcel::pack::PackRule::PackSeparately;
        config.collectors.push_back(std::move(collector));

        auto packed = parcel::pack::RunPack(config);
        REQUIRE(packed);
        REQUIRE(packed.Value().report.bundle_count == 3);
        auto manifest =
            serialize::LoadManifestFile(OutputFile(packed.Value(), "manifest.bytes"), true);
        REQUIRE(manifest);
        const auto* first = manifest.Value().FindAssetByGuid(fixture.material_a_guid);
        const auto* second = manifest.Value().FindAssetByGuid(second_material_guid);
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        REQUIRE(first->bundle_index != second->bundle_index);
        REQUIRE(
            manifest.Value().Bundles()[first->bundle_index].bundle_name.starts_with("separate/"));
        REQUIRE(
            manifest.Value().Bundles()[second->bundle_index].bundle_name.starts_with("separate/"));
    }

    SECTION("PackCollector groups assets under the collector name") {
        auto config = fixture.Config();
        config.collectors.clear();
        auto collector = fixture.Collector("materials", parcel::pack::CollectorType::Main, "a");
        collector.filter_rule = parcel::pack::FilterRule::CollectMaterial;
        collector.pack_rule = parcel::pack::PackRule::PackCollector;
        config.collectors.push_back(std::move(collector));

        auto packed = parcel::pack::RunPack(config);
        REQUIRE(packed);
        REQUIRE(packed.Value().report.bundle_count == 1);
        auto manifest =
            serialize::LoadManifestFile(OutputFile(packed.Value(), "manifest.bytes"), true);
        REQUIRE(manifest);
        const auto* first = manifest.Value().FindAssetByGuid(fixture.material_a_guid);
        const auto* second = manifest.Value().FindAssetByGuid(second_material_guid);
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        REQUIRE(first->bundle_index == second->bundle_index);
        REQUIRE(manifest.Value().Bundles()[first->bundle_index].bundle_name == "materials");
    }

    SECTION("PackGroup groups assets under the configured group") {
        auto config = fixture.Config();
        config.collectors.clear();
        auto collector = fixture.Collector("materials", parcel::pack::CollectorType::Main, "a");
        collector.filter_rule = parcel::pack::FilterRule::CollectMaterial;
        collector.pack_rule = parcel::pack::PackRule::PackGroup;
        collector.group = "ui";
        config.collectors.push_back(std::move(collector));

        auto packed = parcel::pack::RunPack(config);
        REQUIRE(packed);
        REQUIRE(packed.Value().report.bundle_count == 1);
        auto manifest =
            serialize::LoadManifestFile(OutputFile(packed.Value(), "manifest.bytes"), true);
        REQUIRE(manifest);
        const auto* first = manifest.Value().FindAssetByGuid(fixture.material_a_guid);
        REQUIRE(first != nullptr);
        REQUIRE(manifest.Value().Bundles()[first->bundle_index].bundle_name == "ui");
    }

    SECTION("explicit bundle name overrides the pack rule") {
        auto config = fixture.Config();
        config.collectors.clear();
        auto collector = fixture.Collector("materials", parcel::pack::CollectorType::Main, "a");
        collector.filter_rule = parcel::pack::FilterRule::CollectMaterial;
        collector.pack_rule = parcel::pack::PackRule::PackSeparately;
        collector.bundle = "fixed";
        config.collectors.push_back(std::move(collector));

        auto packed = parcel::pack::RunPack(config);
        REQUIRE(packed);
        REQUIRE(packed.Value().report.bundle_count == 1);
        auto manifest =
            serialize::LoadManifestFile(OutputFile(packed.Value(), "manifest.bytes"), true);
        REQUIRE(manifest);
        const auto* first = manifest.Value().FindAssetByGuid(fixture.material_a_guid);
        REQUIRE(first != nullptr);
        REQUIRE(manifest.Value().Bundles()[first->bundle_index].bundle_name == "fixed");
    }
}

TEST_CASE("pack outputNameStyle controls bundle file names") {
    PackFixture fixture;

    SECTION("HashName keeps the legacy GUID file name") {
        auto config = fixture.Config();
        config.output_name_style = 1;
        auto packed = parcel::pack::RunPack(config);
        REQUIRE(packed);
        auto manifest =
            serialize::LoadManifestFile(OutputFile(packed.Value(), "manifest.bytes"), true);
        REQUIRE(manifest);
        for (const auto& bundle : manifest.Value().Bundles()) {
            REQUIRE(bundle.file_name == bundle.bundle_guid.ToHex() + ".bundle");
        }
    }

    SECTION("BundleName uses the sanitized bundle name") {
        auto config = fixture.Config();
        config.output_name_style = 2;
        auto packed = parcel::pack::RunPack(config);
        REQUIRE(packed);
        auto manifest =
            serialize::LoadManifestFile(OutputFile(packed.Value(), "manifest.bytes"), true);
        REQUIRE(manifest);
        const auto* bundle = manifest.Value().FindBundleByName("a");
        REQUIRE(bundle != nullptr);
        REQUIRE(bundle->file_name == "a.bundle");
        const auto* shared = manifest.Value().FindBundleByName("shared/textures");
        REQUIRE(shared != nullptr);
        REQUIRE(shared->file_name == "shared_textures.bundle");
    }

    SECTION("BundleNameHashName includes both name and GUID") {
        auto config = fixture.Config();
        config.output_name_style = 3;
        auto packed = parcel::pack::RunPack(config);
        REQUIRE(packed);
        auto manifest =
            serialize::LoadManifestFile(OutputFile(packed.Value(), "manifest.bytes"), true);
        REQUIRE(manifest);
        const auto* bundle = manifest.Value().FindBundleByName("a");
        REQUIRE(bundle != nullptr);
        REQUIRE(bundle->file_name == "a_" + bundle->bundle_guid.ToHex() + ".bundle");
    }

    SECTION("unknown styles are rejected") {
        auto config = fixture.Config();
        config.output_name_style = 4;
        auto packed = parcel::pack::RunPack(config);
        REQUIRE_FALSE(packed);
        REQUIRE(packed.GetError().code == core::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("pack buildBundleType controls raw archive output") {
    PackFixture fixture;

    SECTION("Archive forces raw assets into archive bundles") {
        auto config = fixture.Config();
        config.build_bundle_type = 3;
        auto packed = parcel::pack::RunPack(config);
        REQUIRE(packed);
        auto manifest =
            serialize::LoadManifestFile(OutputFile(packed.Value(), "manifest.bytes"), true);
        REQUIRE(manifest);
        const auto* raw = manifest.Value().FindAssetByGuid(fixture.raw_guid);
        REQUIRE(raw != nullptr);
        REQUIRE((raw->flags & serialize::kManifestAssetFlagArchiveRaw) != 0);
        const auto* bundle = manifest.Value().FindBundleByName("raw");
        REQUIRE(bundle != nullptr);
        const auto bytes = ReadFile(std::filesystem::path(packed.Value().output_directory) /
                                    "bundles" / std::string(bundle->file_name));
        auto reader = serialize::BundleReader::Open(bytes, true);
        REQUIRE(reader);
        REQUIRE(reader.Value().ReadAll(true));
        REQUIRE(reader.Value().Toc().raw_files.size() == 1);
    }

    SECTION("RawFile is rejected") {
        auto config = fixture.Config();
        config.build_bundle_type = 2;
        auto packed = parcel::pack::RunPack(config);
        REQUIRE_FALSE(packed);
        REQUIRE(packed.GetError().code == core::ErrorCode::Unsupported);
    }

    SECTION("unknown values are rejected") {
        auto config = fixture.Config();
        config.build_bundle_type = 4;
        auto packed = parcel::pack::RunPack(config);
        REQUIRE_FALSE(packed);
        REQUIRE(packed.GetError().code == core::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("pack rejects unsupported collector filter and pack rules") {
    PackFixture fixture;

    SECTION("unsupported filter rule") {
        auto config = fixture.Config();
        config.collectors[0].filter_rule = parcel::pack::FilterRule::CollectPrefab;
        auto packed = parcel::pack::RunPack(config);
        REQUIRE_FALSE(packed);
        REQUIRE(packed.GetError().code == core::ErrorCode::Unsupported);
    }

    SECTION("unsupported pack rule") {
        auto config = fixture.Config();
        config.collectors[0].pack_rule = parcel::pack::PackRule::PackShaderVariants;
        auto packed = parcel::pack::RunPack(config);
        REQUIRE_FALSE(packed);
        REQUIRE(packed.GetError().code == core::ErrorCode::Unsupported);
    }
}

TEST_CASE("pack produces deterministic bundles, shared dependency and manifest") {
    PackFixture fixture;
    auto first = parcel::pack::RunPack(fixture.Config());
    REQUIRE(first);
    const auto& summary = first.Value();
    REQUIRE(summary.report.errors.empty());
    REQUIRE(std::filesystem::path(summary.output_directory) ==
            fixture.output / "windows" / "game" / "1.0.0");
    REQUIRE(summary.report.asset_count == 4);
    REQUIRE(summary.report.main_asset_count == 3);
    REQUIRE(summary.report.bundle_count == 4);
    REQUIRE(summary.report.reused_bundle_count == 0);
    REQUIRE(summary.report.reused_asset_count == 0);

    auto manifest = serialize::LoadManifestFile(OutputFile(summary, "manifest.bytes"), true);
    REQUIRE(manifest);
    REQUIRE(manifest.Value().Assets().size() == 3);
    REQUIRE(manifest.Value().FindAssetByPath("a/" + fixture.material_a_guid.ToHex() + ".prca") !=
            nullptr);
    REQUIRE(manifest.Value().FindAssetByPath("textures/" + fixture.texture_guid.ToHex() +
                                             ".prca") == nullptr);
    const auto* material_a =
        manifest.Value().FindAssetByPath("a/" + fixture.material_a_guid.ToHex() + ".prca");
    REQUIRE(material_a != nullptr);
    REQUIRE(material_a->dependent_bundle_ids.size() == 1);

    const auto* bundle_a = manifest.Value().FindBundleByName("a");
    const auto* bundle_b = manifest.Value().FindBundleByName("b");
    const auto* shared = manifest.Value().FindBundleByName("shared/textures");
    const auto* raw = manifest.Value().FindBundleByName("raw");
    REQUIRE(bundle_a != nullptr);
    REQUIRE(bundle_b != nullptr);
    REQUIRE(shared != nullptr);
    REQUIRE(raw != nullptr);
    REQUIRE(bundle_a->dependent_bundle_ids == std::vector<std::uint32_t>{3});
    REQUIRE(bundle_b->dependent_bundle_ids == std::vector<std::uint32_t>{3});
    REQUIRE(shared->referrer_bundle_ids.size() == 2);

    auto shared_bytes =
        ReadFile(summary.output_directory + "/bundles/" + std::string(shared->file_name));
    auto bundle = serialize::BundleReader::Open(shared_bytes, true);
    REQUIRE(bundle);
    REQUIRE(bundle.Value().Toc().payloads.size() == 1);
    REQUIRE(bundle.Value().Toc().payloads[0].main_asset_guid == fixture.texture_guid);

    REQUIRE(std::filesystem::exists(OutputFile(summary, "manifest.hash")));
    REQUIRE(std::filesystem::exists(OutputFile(summary, "game.version")));
    REQUIRE(std::filesystem::exists(OutputFile(summary, "build_db.json")));
    REQUIRE(std::filesystem::exists(OutputFile(summary, "build_report.json")));

    const auto report_bytes = ReadFile(OutputFile(summary, "build_report.json"));
    const std::string report_text(reinterpret_cast<const char*>(report_bytes.data()),
                                  report_bytes.size());
    auto report_json = assets::ParseJson(report_text);
    REQUIRE(report_json);
    REQUIRE(report_json.Value().Find("assetCount") != nullptr);
    REQUIRE(report_json.Value().Find("assetCount")->number_value == 4);
    REQUIRE(report_json.Value().Find("bundleCount")->number_value == 4);
    REQUIRE(report_json.Value().Find("reusedBundleCount")->number_value == 0);
    REQUIRE(report_json.Value().Find("textureFormats")->array_value.size() == 1);

    const std::vector<std::byte> catalog_bytes =
        ReadFile(OutputFile(summary, "builtin_catalog.bin"));
    auto catalog = serialize::DecodeBuiltinCatalog(catalog_bytes);
    REQUIRE(catalog);
    REQUIRE(catalog.Value().package_name == "game");
    REQUIRE(catalog.Value().package_version == "1.0.0");
    REQUIRE(catalog.Value().entries.size() == 4);
    REQUIRE(report_json.Value().Find("builtinCatalogBytes")->number_value ==
            static_cast<double>(catalog_bytes.size()));
    for (const serialize::BuiltinCatalogEntry& entry : catalog.Value().entries) {
        const auto* bundle_view = manifest.Value().FindBundleByGuid(entry.bundle_guid);
        REQUIRE(bundle_view != nullptr);
        REQUIRE(bundle_view->file_name == entry.file_name);
    }

    std::vector<std::byte> manifest_first = ReadFile(OutputFile(summary, "manifest.bytes"));
    std::vector<std::byte> bundle_a_first =
        ReadFile(summary.output_directory + "/bundles/" + std::string(bundle_a->file_name));
    const auto manifest_hash_bytes = ReadFile(OutputFile(summary, "manifest.hash"));
    std::string manifest_hash_text(reinterpret_cast<const char*>(manifest_hash_bytes.data()),
                                   manifest_hash_bytes.size());
    while (!manifest_hash_text.empty() &&
           (manifest_hash_text.back() == '\n' || manifest_hash_text.back() == '\r')) {
        manifest_hash_text.pop_back();
    }
    auto manifest_hash = core::Hash256::FromHex(manifest_hash_text);
    REQUIRE(manifest_hash);
    REQUIRE(manifest_hash.Value() == core::Sha256(std::span<const std::byte>(manifest_first)
                                                      .subspan(serialize::kManifestHeaderSize)));

    auto second = parcel::pack::RunPack(fixture.Config());
    REQUIRE(second);
    REQUIRE(second.Value().report.reused_bundle_count == second.Value().report.bundle_count);
    REQUIRE(second.Value().report.reused_asset_count == second.Value().report.asset_count);
    REQUIRE(ReadFile(OutputFile(second.Value(), "manifest.bytes")) == manifest_first);
    REQUIRE(ReadFile(OutputFile(second.Value(), "builtin_catalog.bin")) == catalog_bytes);
    REQUIRE(ReadFile(second.Value().output_directory + "/bundles/" +
                     std::string(bundle_a->file_name)) == bundle_a_first);

    auto fresh_output = fixture.output / "fresh";
    auto fresh_config = fixture.Config();
    fresh_config.output_root = ToUtf8(fresh_output);
    auto fresh = parcel::pack::RunPack(fresh_config);
    REQUIRE(fresh);
    REQUIRE(fresh.Value().report.reused_bundle_count == 0);
    REQUIRE(fresh.Value().report.reused_asset_count == 0);
    REQUIRE(ReadFile(OutputFile(fresh.Value(), "manifest.bytes")) == manifest_first);
    REQUIRE(ReadFile(OutputFile(fresh.Value(), "builtin_catalog.bin")) == catalog_bytes);

    auto no_catalog_config = fixture.Config();
    no_catalog_config.output_root = ToUtf8(fixture.output / "no-catalog");
    no_catalog_config.write_builtin_catalog = false;
    auto no_catalog = parcel::pack::RunPack(no_catalog_config);
    REQUIRE(no_catalog);
    REQUIRE_FALSE(std::filesystem::exists(OutputFile(no_catalog.Value(), "builtin_catalog.bin")));
    REQUIRE(no_catalog.Value().report.builtin_catalog_bytes == 0);
}

TEST_CASE("pack verifies written output hashes") {
    PackFixture fixture;
    auto config = fixture.Config();
    config.verify_output_hashes = true;
    auto packed = parcel::pack::RunPack(config);
    REQUIRE(packed);
    REQUIRE(packed.Value().report.output_verified);
    REQUIRE(packed.Value().report.verified_bundle_count == packed.Value().report.bundle_count);

    auto verified = parcel::pack::VerifyPackOutput(packed.Value().output_directory);
    REQUIRE(verified);
    REQUIRE(verified.Value().bundle_count == packed.Value().report.bundle_count);
    REQUIRE(verified.Value().catalog_verified);

    auto manifest = serialize::LoadManifestFile(OutputFile(packed.Value(), "manifest.bytes"), true);
    REQUIRE(manifest);
    REQUIRE_FALSE(manifest.Value().Bundles().empty());
    const std::string bundle_path = packed.Value().output_directory + "/bundles/" +
                                    std::string(manifest.Value().Bundles().front().file_name);
    auto bytes = ReadFile(bundle_path);
    REQUIRE_FALSE(bytes.empty());
    bytes[0] = std::byte{0};
    WriteFile(bundle_path, bytes);

    auto corrupted = parcel::pack::VerifyPackOutput(packed.Value().output_directory);
    REQUIRE_FALSE(corrupted);
    REQUIRE(corrupted.GetError().code == core::ErrorCode::HashMismatch);
}

TEST_CASE("pack reuses unchanged assets after one asset changes") {
    PackFixture fixture;
    auto first = parcel::pack::RunPack(fixture.Config());
    REQUIRE(first);
    const std::vector<std::byte> first_manifest =
        ReadFile(OutputFile(first.Value(), "manifest.bytes"));

    assets::RawFileAsset changed;
    changed.name = "payload";
    changed.data = {std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};
    WriteAsset(fixture.cooked, "raw", "payload", changed, fixture.registry);
    const auto raw_path = fixture.cooked / "raw" / (fixture.raw_guid.ToHex() + ".prca");
    std::filesystem::last_write_time(raw_path, std::filesystem::last_write_time(raw_path) +
                                                   std::chrono::seconds(5));

    auto rebuild = parcel::pack::RunPack(fixture.Config());
    REQUIRE(rebuild);
    REQUIRE(rebuild.Value().report.reused_asset_count == 3);
    REQUIRE(rebuild.Value().report.reused_bundle_count == 3);
    REQUIRE(rebuild.Value().report.bundle_count == 4);
    REQUIRE(ReadFile(OutputFile(rebuild.Value(), "manifest.bytes")) != first_manifest);
}

TEST_CASE("pack keeps a shared dependency inside one referrer bundle") {
    PackFixture fixture;
    assets::TextureAsset texture;
    texture.format = assets::TextureFormat::RGBA8;
    texture.mip_count = 1;
    assets::TextureMip mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.data.assign(4, std::byte{0x20});
    texture.mips.push_back(std::move(mip));
    const AssetGuid merged_texture =
        WriteAsset(fixture.cooked, "textures", "merged-texture", texture, fixture.registry);

    assets::MaterialAsset first;
    first.name = "merged-a";
    first.base_color_texture.texture = merged_texture;
    WriteAsset(fixture.cooked, "merged", "merged-a", first, fixture.registry);
    assets::MaterialAsset second;
    second.name = "merged-b";
    second.base_color_texture.texture = merged_texture;
    WriteAsset(fixture.cooked, "merged", "merged-b", second, fixture.registry);

    auto config = fixture.Config();
    config.collectors.push_back(
        fixture.Collector("main-merged", parcel::pack::CollectorType::Main, "merged"));
    auto result = parcel::pack::RunPack(config);
    REQUIRE(result);

    const std::string texture_path = "textures/" + merged_texture.ToHex() + ".prca";
    const auto found = std::find_if(
        result.Value().report.assets.begin(), result.Value().report.assets.end(),
        [&](const parcel::pack::PackAssetReport& asset) { return asset.path == texture_path; });
    REQUIRE(found != result.Value().report.assets.end());
    REQUIRE(found->bundle == "merged");
}

TEST_CASE("pack supports all bundle compression codecs") {
    PackFixture fixture;
    const std::array<serialize::CompressionCodec, 3> codecs = {
        serialize::CompressionCodec::None,
        serialize::CompressionCodec::Lz4,
        serialize::CompressionCodec::Zstd,
    };
    for (const serialize::CompressionCodec codec : codecs) {
        auto config = fixture.Config();
        config.compression = codec;
        config.output_root =
            ToUtf8(fixture.output / ("codec-" + std::to_string(static_cast<int>(codec))));
        auto result = parcel::pack::RunPack(config);
        REQUIRE(result);
        REQUIRE(result.Value().report.bundle_count == 4);
        const auto bundle_path = std::filesystem::path(result.Value().output_directory) / "bundles";
        std::size_t bundle_count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(bundle_path)) {
            if (entry.is_regular_file() &&
                entry.path().extension() == std::filesystem::path(".bundle")) {
                auto bytes = ReadFile(entry.path());
                auto reader = serialize::BundleReader::Open(bytes, true);
                REQUIRE(reader);
                ++bundle_count;
            }
        }
        REQUIRE(bundle_count == 4);
    }
}

TEST_CASE("pack auto-collects unselected shader assets") {
    PackFixture fixture;
    assets::ShaderLibraryAsset shader;
    shader.name = "auto-shader";
    assets::ShaderBackendLibrary backend;
    backend.backend = assets::ShaderBackend::Dxbc;
    backend.variants.push_back("default");
    shader.backends.push_back(std::move(backend));
    const AssetGuid shader_guid =
        WriteAsset(fixture.cooked, "shaders", "auto-shader", shader, fixture.registry);

    auto config = fixture.Config();
    config.collectors.clear();
    config.collectors.push_back(
        fixture.Collector("main-a", parcel::pack::CollectorType::Main, "a"));
    auto result = parcel::pack::RunPack(config);
    REQUIRE(result);

    const std::string shader_path = "shaders/" + shader_guid.ToHex() + ".prca";
    const auto found = std::find_if(
        result.Value().report.assets.begin(), result.Value().report.assets.end(),
        [&](const parcel::pack::PackAssetReport& asset) { return asset.path == shader_path; });
    REQUIRE(found != result.Value().report.assets.end());
    REQUIRE(found->type == "ShaderLibraryAsset");
    REQUIRE(found->bundle == "shaders");
    REQUIRE(result.Value().report.shader_variants == 1);
}

TEST_CASE("pack rejects missing dependencies") {
    PackFixture fixture;
    assets::MaterialAsset material;
    material.name = "missing";
    material.base_color_texture.texture = GuidFrom("missing-texture");
    WriteAsset(fixture.cooked, "a", "missing-material", material, fixture.registry);

    auto result = parcel::pack::RunPack(fixture.Config());
    REQUIRE_FALSE(result);
    REQUIRE(result.GetError().code == core::ErrorCode::NotFound);
    const auto report_path = fixture.output / "windows" / "game" / "1.0.0" / "build_report.json";
    REQUIRE(std::filesystem::exists(report_path));
    auto report_bytes = ReadFile(report_path);
    const std::string report_text(reinterpret_cast<const char*>(report_bytes.data()),
                                  report_bytes.size());
    auto report = assets::ParseJson(report_text);
    REQUIRE(report);
    REQUIRE_FALSE(report.Value().Find("errors")->array_value.empty());
}

TEST_CASE("pack rejects cyclic asset dependencies") {
    PackFixture fixture;
    const AssetGuid first_guid = GuidFrom("cycle-a");
    const AssetGuid second_guid = GuidFrom("cycle-b");
    assets::MaterialAsset first;
    first.name = "cycle-a";
    first.base_color_texture.texture = second_guid;
    assets::MaterialAsset second;
    second.name = "cycle-b";
    second.base_color_texture.texture = first_guid;
    auto first_bytes = assets::EncodeAssetBytes(first, fixture.registry);
    auto second_bytes = assets::EncodeAssetBytes(second, fixture.registry);
    REQUIRE(first_bytes);
    REQUIRE(second_bytes);
    WriteFile(fixture.cooked / "a" / (first_guid.ToHex() + ".prca"), first_bytes.Value());
    WriteFile(fixture.cooked / "a" / (second_guid.ToHex() + ".prca"), second_bytes.Value());

    auto result = parcel::pack::RunPack(fixture.Config());
    REQUIRE_FALSE(result);
    REQUIRE(result.GetError().code == core::ErrorCode::InvalidArgument);
}

TEST_CASE("pack rejects cyclic bundle dependencies") {
    PackFixture fixture;
    const AssetGuid a1 = GuidFrom("bundle-cycle-a1");
    const AssetGuid a2 = GuidFrom("bundle-cycle-a2");
    const AssetGuid b1 = GuidFrom("bundle-cycle-b1");
    assets::MaterialAsset material_a1;
    material_a1.name = "bundle-cycle-a1";
    material_a1.base_color_texture.texture = b1;
    assets::MaterialAsset material_a2;
    material_a2.name = "bundle-cycle-a2";
    assets::MaterialAsset material_b1;
    material_b1.name = "bundle-cycle-b1";
    material_b1.base_color_texture.texture = a2;
    auto bytes_a1 = assets::EncodeAssetBytes(material_a1, fixture.registry);
    auto bytes_a2 = assets::EncodeAssetBytes(material_a2, fixture.registry);
    auto bytes_b1 = assets::EncodeAssetBytes(material_b1, fixture.registry);
    REQUIRE(bytes_a1);
    REQUIRE(bytes_a2);
    REQUIRE(bytes_b1);
    WriteFile(fixture.cooked / "a" / (a1.ToHex() + ".prca"), bytes_a1.Value());
    WriteFile(fixture.cooked / "a" / (a2.ToHex() + ".prca"), bytes_a2.Value());
    WriteFile(fixture.cooked / "b" / (b1.ToHex() + ".prca"), bytes_b1.Value());

    auto result = parcel::pack::RunPack(fixture.Config());
    REQUIRE_FALSE(result);
    REQUIRE(result.GetError().code == core::ErrorCode::InvalidArgument);
}

TEST_CASE("pack maps parcelmeta source paths and groups subassets") {
    PackFixture fixture;
    assets::TextureAsset grouped_texture;
    grouped_texture.format = assets::TextureFormat::RGBA8;
    grouped_texture.sRGB = true;
    grouped_texture.mip_count = 1;
    assets::TextureMip grouped_mip;
    grouped_mip.width = 2;
    grouped_mip.height = 2;
    grouped_mip.row_pitch = 8;
    grouped_mip.data.assign(16, std::byte{0x33});
    grouped_texture.mips.push_back(std::move(grouped_mip));
    const AssetGuid texture_guid = WriteAsset(fixture.cooked, "group-deps", "group-texture",
                                              grouped_texture, fixture.registry);

    assets::MaterialAsset grouped_material;
    grouped_material.name = "group-material";
    grouped_material.base_color_texture.texture = texture_guid;
    const AssetGuid material_guid = WriteAsset(fixture.cooked, "group-main", "group-material",
                                               grouped_material, fixture.registry);

    const AssetGuid source_guid = GuidFrom("group-source");
    const std::string zero_hash = "sha256:" + std::string(64, '0');
    const std::string meta_text =
        "{\n"
        "  \"version\": 1,\n"
        "  \"sourceGuid\": \"" +
        source_guid.ToHex() +
        "\",\n"
        "  \"importer\": \"fbx\",\n"
        "  \"importerVersion\": 8,\n"
        "  \"sourceHash\": \"" +
        zero_hash +
        "\",\n"
        "  \"settingsHash\": \"" +
        zero_hash +
        "\",\n"
        "  \"settings\": {},\n"
        "  \"assets\": [\n"
        "    {\"guid\": \"" +
        material_guid.ToHex() +
        "\", \"type\": \"MaterialAsset\", \"role\": \"scene\", \"localId\": \"scene\"},\n"
        "    {\"guid\": \"" +
        texture_guid.ToHex() +
        "\", \"type\": \"TextureAsset\", \"role\": \"texture\", \"localId\": \"texture\"}\n"
        "  ],\n"
        "  \"dependencies\": [\"" +
        texture_guid.ToHex() +
        "\"]\n"
        "}\n";
    WriteText(fixture.project / "assets" / "hero.fbx.parcelmeta", meta_text);

    auto config = fixture.Config();
    config.source_root = "assets";
    config.single_referenced_pack_alone = true;
    config.collectors.push_back(
        fixture.Collector("group-main", parcel::pack::CollectorType::Main, "group-main"));

    auto packed = parcel::pack::RunPack(config);
    REQUIRE(packed);
    const auto& summary = packed.Value();
    REQUIRE(summary.report.asset_count == 6);
    REQUIRE(summary.report.main_asset_count == 4);
    REQUIRE(summary.report.bundle_count == 5);

    const auto material_report = std::find_if(
        summary.report.assets.begin(), summary.report.assets.end(),
        [&](const parcel::pack::PackAssetReport& asset) { return asset.guid == material_guid; });
    REQUIRE(material_report != summary.report.assets.end());
    CHECK(material_report->path == "group-main/" + material_guid.ToHex() + ".prca");
    CHECK(material_report->source_path == "assets/hero.fbx");
    CHECK(material_report->bundle == "assets");

    const auto texture_report = std::find_if(
        summary.report.assets.begin(), summary.report.assets.end(),
        [&](const parcel::pack::PackAssetReport& asset) { return asset.guid == texture_guid; });
    REQUIRE(texture_report != summary.report.assets.end());
    CHECK(texture_report->path == "group-deps/" + texture_guid.ToHex() + ".prca");
    CHECK(texture_report->source_path == "assets/hero.fbx");
    CHECK(texture_report->bundle == "assets");

    const auto group_bundle = std::find_if(
        summary.report.bundles.begin(), summary.report.bundles.end(),
        [](const parcel::pack::PackBundleReport& bundle) { return bundle.name == "assets"; });
    REQUIRE(group_bundle != summary.report.bundles.end());
    CHECK(group_bundle->asset_count == 2);

    auto manifest = serialize::LoadManifestFile(OutputFile(summary, "manifest.bytes"), true);
    REQUIRE(manifest);
    REQUIRE(manifest.Value().Assets().size() == 4);
    const auto* packed_material = manifest.Value().FindAssetByPath("assets/hero.fbx");
    REQUIRE(packed_material != nullptr);
    CHECK(packed_material->address == "assets/hero.fbx");
    CHECK(manifest.Value().FindAssetByPath("group-main/" + material_guid.ToHex() + ".prca") ==
          nullptr);

    auto physical_config = config;
    physical_config.use_source_path = false;
    physical_config.force_sub_asset_grouping = false;
    physical_config.output_root = ToUtf8(fixture.directory.Path() / "physical-build");
    auto physical = parcel::pack::RunPack(physical_config);
    REQUIRE(physical);
    auto physical_manifest =
        serialize::LoadManifestFile(OutputFile(physical.Value(), "manifest.bytes"), true);
    REQUIRE(physical_manifest);
    CHECK(physical_manifest.Value().FindAssetByPath("group-main/" + material_guid.ToHex() +
                                                    ".prca") != nullptr);
    const auto physical_texture = std::find_if(
        physical.Value().report.assets.begin(), physical.Value().report.assets.end(),
        [&](const parcel::pack::PackAssetReport& asset) { return asset.guid == texture_guid; });
    REQUIRE(physical_texture != physical.Value().report.assets.end());
    CHECK(physical_texture->bundle == "shared/group-deps");
}

TEST_CASE("pack rejects multiple raw assets in one bundle") {
    PackFixture fixture;
    assets::RawFileAsset second_raw;
    second_raw.name = "second";
    second_raw.data = {std::byte{9}};
    WriteAsset(fixture.cooked, "raw", "second-raw", second_raw, fixture.registry);

    auto result = parcel::pack::RunPack(fixture.Config());
    REQUIRE_FALSE(result);
    REQUIRE(result.GetError().code == core::ErrorCode::InvalidArgument);
}
