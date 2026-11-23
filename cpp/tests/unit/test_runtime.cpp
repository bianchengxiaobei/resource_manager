#include <catch_amalgamated.hpp>

#include "parcel/assets/asset_codec.hpp"
#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/raw_file_asset.hpp"
#include "parcel/assets/scene_asset.hpp"
#include "parcel/assets/serializers.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/guid.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/pack/pack.hpp"
#include "parcel/runtime/runtime.hpp"
#include "parcel/serialize/asset_file.hpp"
#include "parcel/serialize/builtin_catalog.hpp"
#include "parcel/serialize/bundle.hpp"
#include "parcel/serialize/manifest.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace parcel;
using parcel::core::AssetGuid;
using parcel::core::Hash256;
using parcel::test::TempDir;
using parcel::test::ToUtf8;

namespace {

AssetGuid GuidFrom(std::string_view name) {
    const Hash256 hash = core::Sha256("parcel.runtime.test:" + std::string(name));
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

AssetGuid WriteAsset(const std::filesystem::path& root, std::string_view folder,
                     std::string_view name, const assets::AssetObject& object,
                     const assets::TypeRegistry& registry) {
    const AssetGuid guid = GuidFrom(name);
    auto bytes = assets::EncodeAssetBytes(object, registry);
    REQUIRE(bytes);
    WriteFile(root / std::u8string(folder.begin(), folder.end()) / (guid.ToHex() + ".prca"),
              bytes.Value());
    return guid;
}

class CountingSceneLoader final : public scene::ISceneLoader {
  public:
    core::Result<scene::SceneInstance> Instantiate(const scene::SceneData& data,
                                                   void* registry) override {
        ++instantiate_count;
        last_registry = registry;
        scene::SceneInstance instance;
        instance.registry = registry;
        for (const scene::SceneEntityRecord& entity : data.entities) {
            instance.entities.push_back(entity.id);
        }
        return instance;
    }

    core::Result<void> Destroy(scene::SceneInstance& instance) override {
        ++destroy_count;
        instance.entities.clear();
        instance.registry = nullptr;
        return {};
    }

    int instantiate_count = 0;
    int destroy_count = 0;
    void* last_registry = nullptr;
};

class FailingSceneLoader final : public scene::ISceneLoader {
  public:
    core::Result<scene::SceneInstance> Instantiate(const scene::SceneData&, void*) override {
        ++instantiate_count;
        return core::MakeError(core::ErrorCode::Corrupt, "scene instantiate failed");
    }

    core::Result<void> Destroy(scene::SceneInstance&) override { return {}; }

    int instantiate_count = 0;
};

struct RuntimeFixture {
    TempDir directory{"runtime"};
    std::filesystem::path project = directory.Path() / "project";
    std::filesystem::path cooked = project / "cooked";
    std::filesystem::path output = directory.Path() / "build";
    assets::TypeRegistry registry;
    AssetGuid material_guid;
    AssetGuid texture_guid;
    AssetGuid scene_guid;
    AssetGuid raw_guid;
    std::string output_directory;
    std::string material_path;
    std::string scene_path;
    std::string raw_path;
    std::vector<std::string> bulk_raw_paths;

    explicit RuntimeFixture(std::size_t extra_raw_assets = 0) {
        std::filesystem::create_directories(cooked);
        REQUIRE(assets::RegisterStandardAssetSerializers(registry));

        assets::TextureAsset texture;
        texture.format = assets::TextureFormat::RGBA8;
        texture.sRGB = true;
        texture.mip_count = 1;
        assets::TextureMip mip;
        mip.width = 2;
        mip.height = 2;
        mip.row_pitch = 8;
        mip.data.assign(16, std::byte{0x3c});
        texture.mips.push_back(std::move(mip));
        texture_guid = WriteAsset(cooked, "textures", "texture", texture, registry);

        assets::MaterialAsset material;
        material.name = "runtime-material";
        material.base_color_texture.texture = texture_guid;
        material_guid = WriteAsset(cooked, "a", "material", material, registry);

        assets::SceneAsset scene_asset;
        scene_asset.name = "runtime-scene";
        scene::SceneEntityRecord root;
        root.id = scene::SceneEntityId{1};
        root.name = "root";
        scene_asset.scene.entities.push_back(root);
        scene_guid = WriteAsset(cooked, "a", "scene", scene_asset, registry);

        assets::RawFileAsset raw;
        raw.name = "payload";
        raw.data = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        raw_guid = WriteAsset(cooked, "raw", "payload", raw, registry);

        for (std::size_t i = 0; i < extra_raw_assets; ++i) {
            assets::RawFileAsset bulk;
            bulk.name = "bulk-" + std::to_string(i);
            bulk.data = {std::byte{1}, std::byte{2}, std::byte{3}};
            const AssetGuid guid = WriteAsset(cooked, "bulk", "bulk-" + std::to_string(i), bulk,
                                              registry);
            bulk_raw_paths.push_back("bulk/" + guid.ToHex() + ".prca");
        }


        pack::PackConfig config;
        config.project_root = ToUtf8(project);
        config.cooked_root = ToUtf8(cooked);
        config.output_root = ToUtf8(output);
        config.package_name = "game";
        config.package_version = "1.0.0";
        config.target = "windows";
        config.jobs = 2;

        pack::CollectorConfig main_collector;
        main_collector.name = "main";
        main_collector.type = pack::CollectorType::Main;
        main_collector.path = "a";
        main_collector.address_rule = pack::AddressRule::AssetPath;
        config.collectors.push_back(main_collector);

        pack::CollectorConfig texture_collector;
        texture_collector.name = "textures";
        texture_collector.type = pack::CollectorType::Depend;
        texture_collector.path = "textures";
        texture_collector.address_rule = pack::AddressRule::AssetPath;
        config.collectors.push_back(texture_collector);

        pack::CollectorConfig raw_collector;
        raw_collector.name = "raw";
        raw_collector.type = pack::CollectorType::Main;
        raw_collector.path = "raw";
        raw_collector.address_rule = pack::AddressRule::AssetPath;
        raw_collector.bundle = "raw";
        config.collectors.push_back(raw_collector);

        for (std::size_t i = 0; i < bulk_raw_paths.size(); ++i) {
            pack::CollectorConfig bulk_collector;
            bulk_collector.name = "bulk-" + std::to_string(i);
            bulk_collector.type = pack::CollectorType::Main;
            bulk_collector.files = {bulk_raw_paths[i]};
            bulk_collector.address_rule = pack::AddressRule::AssetPath;
            bulk_collector.bundle = "bulk-" + std::to_string(i);
            config.collectors.push_back(std::move(bulk_collector));
        }

        auto result = pack::RunPack(config);
        if (!result) {
            std::fprintf(stderr, "pack failed: %s\n", result.GetError().ToString().c_str());
        }
        REQUIRE(result);
        output_directory = result.Value().output_directory;
        material_path = "a/" + material_guid.ToHex() + ".prca";
        scene_path = "a/" + scene_guid.ToHex() + ".prca";
        raw_path = "raw/" + raw_guid.ToHex() + ".prca";
    }

    runtime::ResourcePackageOptions PackageOptions(std::string_view builtin_root = {}) const {
        runtime::ResourcePackageOptions options;
        options.package_name = "game";
        options.builtin_root = builtin_root.empty() ? output_directory : std::string(builtin_root);
        options.verify_bundle_on_load = true;
        return options;
    }
};

void Pump(runtime::AssetSystem& system, runtime::AssetHandle& handle, int max_frames = 2000) {
    for (int i = 0; i < max_frames && !handle.IsDone(); ++i) {
        system.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

serialize::ManifestDesc CopyManifest(const serialize::ManifestView& view) {
    serialize::ManifestDesc desc;
    desc.package_name = std::string(view.PackageName());
    desc.package_version = std::string(view.PackageVersion());
    desc.flags = view.Header().flags;
    desc.output_name_style = view.Header().output_name_style;
    desc.build_bundle_type = view.Header().build_bundle_type;
    for (const auto tag : view.Tags()) {
        desc.tags.emplace_back(tag);
    }
    for (const auto& bundle : view.Bundles()) {
        serialize::ManifestBundleDesc copy;
        copy.bundle_guid = bundle.bundle_guid;
        copy.bundle_name = std::string(bundle.bundle_name);
        copy.file_name = std::string(bundle.file_name);
        copy.file_size = bundle.file_size;
        copy.file_sha256 = bundle.file_sha256;
        copy.file_crc = bundle.file_crc;
        copy.flags = bundle.flags;
        copy.dependent_bundle_ids = bundle.dependent_bundle_ids;
        copy.referrer_bundle_ids = bundle.referrer_bundle_ids;
        for (const auto tag : bundle.tags) {
            copy.tags.emplace_back(tag);
        }
        desc.bundles.push_back(std::move(copy));
    }
    for (const auto& asset : view.Assets()) {
        serialize::ManifestAssetDesc copy;
        copy.asset_guid = asset.asset_guid;
        copy.address = std::string(asset.address);
        copy.asset_path = std::string(asset.asset_path);
        copy.type_name = std::string(asset.type_name);
        copy.bundle_id = asset.bundle_index;
        copy.payload_id = asset.payload_id;
        copy.local_object_id = asset.local_object_id;
        copy.dependent_bundle_ids = asset.dependent_bundle_ids;
        copy.flags = asset.flags;
        for (const auto tag : asset.tags) {
            copy.tags.emplace_back(tag);
        }
        desc.assets.push_back(std::move(copy));
    }
    return desc;
}

} // namespace

TEST_CASE("runtime initializes packages and loads assets synchronously", "[runtime]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    runtime::AssetSystemOptions options;
    options.worker_threads = 2;
    REQUIRE(system.Initialize(options));

    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);
    REQUIRE(system.GetPackage("game") == package.Value());
    REQUIRE(package.Value()->Version() == "1.0.0");
    REQUIRE(package.Value()->Manifest() != nullptr);

    auto handle = package.Value()->LoadAssetSync<assets::MaterialAsset>(fixture.material_path);
    INFO(handle.LastError().ToString());
    REQUIRE(handle.IsValid());
    REQUIRE(handle.IsDone());
    REQUIRE(handle.Status() == core::OperationStatus::Succeeded);
    auto* material = handle.Get<assets::MaterialAsset>();
    REQUIRE(material != nullptr);
    REQUIRE(material->name == "runtime-material");
    REQUIRE(material->base_color_texture.texture == fixture.texture_guid);
    REQUIRE(handle.Info().asset_guid == fixture.material_guid);

    auto all = package.Value()->LoadAllAssetsSync<assets::MaterialAsset>(fixture.material_path);
    REQUIRE(all.IsDone());
    REQUIRE(all.Status() == core::OperationStatus::Succeeded);
    REQUIRE(all.Objects().size() == 1);

    REQUIRE(package.Value()->IsLocationValid(fixture.material_path));
    REQUIRE(package.Value()->GetAssetInfo(fixture.material_path).has_value());
    REQUIRE(package.Value()->GetAllAssetInfos().size() == 3);

    handle.Release();
    all.Release();
    (void)package.Value()->UnloadUnusedAssets();
    REQUIRE(system.DestroyPackage("game"));
    REQUIRE(system.GetPackage("game") == nullptr);
    system.Shutdown();
}

TEST_CASE("runtime loads asynchronously and deduplicates providers", "[runtime]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    auto first = package.Value()->LoadAssetAsync<assets::MaterialAsset>(fixture.material_path);
    auto second = package.Value()->LoadAssetAsync<assets::MaterialAsset>(fixture.material_path);
    Pump(system, first);
    Pump(system, second);
    INFO(first.LastError().ToString());
    INFO(second.LastError().ToString());
    REQUIRE(first.Status() == core::OperationStatus::Succeeded);
    REQUIRE(second.Status() == core::OperationStatus::Succeeded);
    REQUIRE(first.GetObject() == second.GetObject());
    REQUIRE(package.Value()->Stats().providers <= 1);
    REQUIRE(package.Value()->Stats().handles >= 2);

    auto callback_count = 0;
    auto third = package.Value()->LoadAssetAsync<assets::MaterialAsset>(fixture.material_path);
    third.OnComplete([&callback_count](const runtime::AssetHandle&) { ++callback_count; });
    Pump(system, third);
    REQUIRE(callback_count == 1);
    third.OnComplete([&callback_count] { ++callback_count; });
    REQUIRE(callback_count == 2);

    first.Release();
    second.Release();
    third.Release();
    (void)package.Value()->UnloadAllAssets();
    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime loads raw and bundle files", "[runtime]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    auto raw = package.Value()->LoadRawFile(fixture.raw_path);
    INFO(raw.LastError().ToString());
    REQUIRE(raw.Status() == core::OperationStatus::Succeeded);
    REQUIRE(raw.Data().size() == 4);
    REQUIRE(raw.Data()[0] == std::byte{1});
    REQUIRE(raw.Data()[3] == std::byte{4});

    auto bundle = package.Value()->LoadBundleFile("raw");
    REQUIRE(bundle.Status() == core::OperationStatus::Succeeded);
    REQUIRE(bundle.Data().size() >= 128);
    const auto* bytes = reinterpret_cast<const char*>(bundle.Data().data());
    REQUIRE(std::string_view(bytes, 4) == "PRCL");

    raw.Release();
    bundle.Release();
    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("sandbox app footprint cleans cache after an overwrite install", "[runtime]") {
    TempDir directory{"runtime_footprint"};
    const std::filesystem::path cache = directory.Path() / "cache";
    const std::vector<std::byte> payload{std::byte{1}, std::byte{2}, std::byte{3}};

    runtime::FileSystemOptions first;
    first.package_name = "game";
    first.root = ToUtf8(cache);
    first.app_build_id = "build-1";
    first.install_cleanup_mode = runtime::InstallCleanupMode::All;
    {
        runtime::SandboxFileSystem sandbox(first);
        REQUIRE(sandbox.Initialize({}));
        WriteFile(cache / "bundles" / "bundle.bundle", payload);
        WriteFile(cache / "temp" / "bundle.part", payload);
        WriteFile(cache / "manifest.bytes", payload);
        WriteFile(cache / "game.version", payload);
        WriteFile(cache / "unrelated.txt", payload);
    }

    const auto footprint_path = cache / runtime::kApplicationFootprintFileName;
    REQUIRE(std::filesystem::exists(footprint_path));
    const auto first_footprint = ReadFile(footprint_path);
    REQUIRE(std::string(reinterpret_cast<const char*>(first_footprint.data()),
                        first_footprint.size()) == "build-1");

    runtime::FileSystemOptions second = first;
    second.app_build_id = "build-2";
    runtime::SandboxFileSystem overwritten(second);
    REQUIRE(overwritten.Initialize({}));
    REQUIRE_FALSE(std::filesystem::exists(cache / "bundles" / "bundle.bundle"));
    REQUIRE_FALSE(std::filesystem::exists(cache / "temp" / "bundle.part"));
    REQUIRE_FALSE(std::filesystem::exists(cache / "manifest.bytes"));
    REQUIRE_FALSE(std::filesystem::exists(cache / "game.version"));
    REQUIRE_FALSE(std::filesystem::exists(cache / "unrelated.txt"));
    const auto second_footprint = ReadFile(footprint_path);
    REQUIRE(std::string(reinterpret_cast<const char*>(second_footprint.data()),
                        second_footprint.size()) == "build-2");
}

TEST_CASE("sandbox app footprint supports selective cleanup modes", "[runtime]") {
    TempDir directory{"runtime_footprint_modes"};
    const std::filesystem::path cache = directory.Path() / "cache";
    const std::vector<std::byte> payload{std::byte{1}};

    runtime::FileSystemOptions initial;
    initial.package_name = "game";
    initial.root = ToUtf8(cache);
    initial.app_build_id = "build-1";
    initial.install_cleanup_mode = runtime::InstallCleanupMode::Bundles;
    {
        runtime::SandboxFileSystem sandbox(initial);
        REQUIRE(sandbox.Initialize({}));
        WriteFile(cache / "bundles" / "nested.bundle", payload);
        WriteFile(cache / "direct.bundle", payload);
        WriteFile(cache / "direct.bundle.cache", payload);
        WriteFile(cache / "temp" / "orphan.part", payload);
        WriteFile(cache / "manifest.bytes", payload);
        WriteFile(cache / "game.version", payload);
        WriteFile(cache / "manifest.hash", payload);
    }

    runtime::FileSystemOptions bundles = initial;
    bundles.app_build_id = "build-2";
    runtime::SandboxFileSystem bundle_cleanup(bundles);
    REQUIRE(bundle_cleanup.Initialize({}));
    REQUIRE_FALSE(std::filesystem::exists(cache / "bundles" / "nested.bundle"));
    REQUIRE_FALSE(std::filesystem::exists(cache / "direct.bundle"));
    REQUIRE_FALSE(std::filesystem::exists(cache / "direct.bundle.cache"));
    REQUIRE_FALSE(std::filesystem::exists(cache / "temp" / "orphan.part"));
    REQUIRE(std::filesystem::exists(cache / "manifest.bytes"));
    REQUIRE(std::filesystem::exists(cache / "game.version"));

    runtime::FileSystemOptions manifests = initial;
    manifests.app_build_id = "build-3";
    manifests.install_cleanup_mode = runtime::InstallCleanupMode::Manifests;
    runtime::SandboxFileSystem manifest_cleanup(manifests);
    REQUIRE(manifest_cleanup.Initialize({}));
    REQUIRE_FALSE(std::filesystem::exists(cache / "manifest.bytes"));
    REQUIRE_FALSE(std::filesystem::exists(cache / "game.version"));
    REQUIRE_FALSE(std::filesystem::exists(cache / "manifest.hash"));
}

TEST_CASE("sandbox cache detects and removes corrupt bundles", "[runtime]") {
    RuntimeFixture fixture;
    const std::filesystem::path cache = fixture.directory.Path() / "cache";
    std::error_code error;
    std::filesystem::create_directories(cache, error);
    REQUIRE_FALSE(error);
    for (const auto& entry : std::filesystem::directory_iterator(fixture.output_directory)) {
        std::filesystem::copy(entry.path(), cache / entry.path().filename(),
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              error);
        REQUIRE_FALSE(error);
    }

    auto manifest = serialize::LoadManifestFile(ToUtf8(cache / "manifest.bytes"));
    REQUIRE(manifest);
    const auto* bundle = manifest.Value().FindBundleByName("a");
    REQUIRE(bundle != nullptr);
    const auto bundle_path = cache / "bundles" / std::string(bundle->file_name);
    REQUIRE(std::filesystem::exists(bundle_path));
    auto bytes = ReadFile(bundle_path);
    REQUIRE(bytes.size() > 128);
    bytes[128] ^= std::byte{0xff};
    WriteFile(bundle_path, bytes);

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    runtime::ResourcePackageOptions options;
    options.package_name = "game";
    options.sandbox_root = ToUtf8(cache);
    auto package = system.CreatePackage(options);
    REQUIRE(package);

    auto handle = package.Value()->LoadAsset<assets::MaterialAsset>(fixture.material_path);
    REQUIRE_FALSE((handle.IsDone() && handle.Status() == core::OperationStatus::Succeeded));
    REQUIRE_FALSE(handle.LastError().IsOk());
    REQUIRE_FALSE(std::filesystem::exists(bundle_path));

    handle.Release();
    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime reports unsupported scene and downloader APIs", "[runtime]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    auto scene = package.Value()->LoadScene("missing-scene");
    REQUIRE(scene.Status() == core::OperationStatus::Failed);
    REQUIRE(scene.LastError().code == core::ErrorCode::Unsupported);

    auto downloader = package.Value()->CreateDownloader();
    REQUIRE_FALSE(downloader);
    REQUIRE(downloader.GetError().code == core::ErrorCode::Unsupported);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("builtin file system loads catalog and falls back when absent", "[runtime][builtin]") {
    RuntimeFixture fixture;
    const std::string catalog_path = fixture.output_directory + "/builtin_catalog.bin";

    runtime::FileSystemOptions options;
    options.package_name = "game";
    options.root = fixture.output_directory;
    runtime::BuiltinFileSystem catalog_fs(options);
    REQUIRE(catalog_fs.Initialize({}));
    REQUIRE(catalog_fs.HasCatalog());
    auto manifest = catalog_fs.LoadManifest();
    REQUIRE(manifest);
    REQUIRE_FALSE(manifest.Value()->Bundles().empty());
    REQUIRE(catalog_fs.CatalogEntryCount() == manifest.Value()->Bundles().size());
    for (const auto& bundle : manifest.Value()->Bundles()) {
        REQUIRE(catalog_fs.CanAccept(bundle));
    }

    const std::vector<std::byte> catalog_bytes = ReadFile(catalog_path);
    auto catalog = serialize::DecodeBuiltinCatalog(catalog_bytes);
    REQUIRE(catalog);
    catalog.Value().package_name = "other";
    auto mismatched = serialize::EncodeBuiltinCatalog(catalog.Value());
    REQUIRE(mismatched);
    WriteFile(catalog_path, mismatched.Value());
    runtime::BuiltinFileSystem mismatched_fs(options);
    auto mismatch_result = mismatched_fs.Initialize({});
    REQUIRE_FALSE(mismatch_result);
    REQUIRE(mismatch_result.GetError().code == core::ErrorCode::Corrupt);

    REQUIRE(std::filesystem::remove(catalog_path));
    runtime::BuiltinFileSystem fallback_fs(options);
    REQUIRE(fallback_fs.Initialize({}));
    REQUIRE_FALSE(fallback_fs.HasCatalog());
    for (const auto& bundle : manifest.Value()->Bundles()) {
        REQUIRE(fallback_fs.CanAccept(bundle));
    }
}

TEST_CASE("runtime scene load instantiates and unload destroys", "[runtime][scene]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    runtime::ResourcePackageOptions options = fixture.PackageOptions();
    options.auto_unload_bundle_when_unused = true;
    auto package = system.CreatePackage(options);
    REQUIRE(package);

    int registry_token = 0;
    auto loader = std::make_shared<CountingSceneLoader>();
    package.Value()->SetSceneLoader(loader, &registry_token);

    auto first = package.Value()->LoadScene(fixture.scene_path);
    REQUIRE(first.Status() == core::OperationStatus::Succeeded);
    REQUIRE(loader->instantiate_count == 1);
    REQUIRE(loader->last_registry == &registry_token);
    REQUIRE(first.Get<assets::SceneAsset>() != nullptr);

    auto second = package.Value()->LoadScene(fixture.scene_path);
    REQUIRE(second.Status() == core::OperationStatus::Succeeded);
    REQUIRE(loader->instantiate_count == 1);

    REQUIRE(package.Value()->UnloadScene(first));
    REQUIRE_FALSE(first.IsValid());
    REQUIRE(loader->destroy_count == 0);

    REQUIRE(package.Value()->UnloadScene(second));
    REQUIRE_FALSE(second.IsValid());
    REQUIRE(loader->destroy_count == 1);
    REQUIRE(package.Value()->Stats().handles == 0);
    REQUIRE(package.Value()->Stats().loaded_bundles == 0);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime asynchronous scene load instantiates on update", "[runtime][scene]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    auto loader = std::make_shared<CountingSceneLoader>();
    package.Value()->SetSceneLoader(loader, nullptr);
    auto handle = package.Value()->LoadSceneAsync(fixture.scene_path);
    Pump(system, handle);
    INFO(handle.LastError().ToString());
    REQUIRE(handle.Status() == core::OperationStatus::Succeeded);
    REQUIRE(loader->instantiate_count == 1);

    REQUIRE(package.Value()->UnloadScene(handle));
    REQUIRE(loader->destroy_count == 1);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime destroy package unloads loaded scenes", "[runtime][scene]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    auto loader = std::make_shared<CountingSceneLoader>();
    package.Value()->SetSceneLoader(loader, nullptr);
    auto handle = package.Value()->LoadScene(fixture.scene_path);
    REQUIRE(handle.Status() == core::OperationStatus::Succeeded);
    REQUIRE(loader->instantiate_count == 1);
    REQUIRE(loader->destroy_count == 0);

    REQUIRE(system.DestroyPackage("game"));
    REQUIRE(loader->destroy_count == 1);
    system.Shutdown();
}

TEST_CASE("runtime scene load rejects non-scenes and loader failures", "[runtime][scene]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    auto loader = std::make_shared<CountingSceneLoader>();
    package.Value()->SetSceneLoader(loader, nullptr);
    auto material = package.Value()->LoadScene(fixture.material_path);
    REQUIRE(material.Status() == core::OperationStatus::Failed);
    REQUIRE(material.LastError().code == core::ErrorCode::InvalidArgument);
    REQUIRE(loader->instantiate_count == 0);
    material.Release();

    auto failing = std::make_shared<FailingSceneLoader>();
    package.Value()->SetSceneLoader(failing, nullptr);
    auto scene = package.Value()->LoadScene(fixture.scene_path);
    REQUIRE(scene.Status() == core::OperationStatus::Failed);
    REQUIRE(scene.LastError().code == core::ErrorCode::Corrupt);
    REQUIRE(failing->instantiate_count == 1);
    scene.Release();

    (void)package.Value()->UnloadUnusedAssets();
    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime decodes all objects and sub-assets", "[runtime]") {
    TempDir directory{"runtime_multi"};
    const std::filesystem::path package_root = directory.Path() / "package";
    std::filesystem::create_directories(package_root / "bundles");

    assets::TypeRegistry registry;
    REQUIRE(assets::RegisterStandardAssetSerializers(registry));
    assets::MaterialAsset material;
    material.name = "multi-main";
    assets::TextureAsset sub_texture;
    sub_texture.format = assets::TextureFormat::RGBA8;
    sub_texture.mip_count = 1;
    assets::TextureMip mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.data.assign(4, std::byte{0x11});
    sub_texture.mips.push_back(std::move(mip));

    auto main_desc = assets::EncodeAsset(material, registry);
    auto sub_desc = assets::EncodeAsset(sub_texture, registry);
    REQUIRE(main_desc);
    REQUIRE(sub_desc);
    serialize::AssetFileDesc asset_file;
    asset_file.types = main_desc.Value().types;
    asset_file.types.push_back(sub_desc.Value().types[0]);
    asset_file.objects.push_back(main_desc.Value().objects[0]);
    asset_file.blobs = main_desc.Value().blobs;
    auto sub_object = sub_desc.Value().objects[0];
    const std::uint64_t blob_offset = asset_file.blobs.size();
    if (sub_object.blob_id != serialize::kNoBlob) {
        sub_object.blob_id += blob_offset;
    }
    asset_file.objects.push_back(std::move(sub_object));
    asset_file.blobs.insert(asset_file.blobs.end(), sub_desc.Value().blobs.begin(),
                            sub_desc.Value().blobs.end());
    asset_file.objects[0].local_id = 0;
    asset_file.objects[1].local_id = 1;
    asset_file.sub_assets.push_back(serialize::AssetSubAssetDesc{"sub-texture", 1, 0});
    auto asset_bytes = serialize::WriteAssetFile(asset_file);
    REQUIRE(asset_bytes);

    const AssetGuid material_guid = GuidFrom("multi-material");
    const AssetGuid bundle_guid = GuidFrom("multi-bundle");
    serialize::BundleToc toc;
    toc.bundle_guid = bundle_guid;
    toc.bundle_name = "multi";
    serialize::BundlePayloadDesc payload;
    payload.payload_id = 0;
    payload.logical_offset = 0;
    payload.size = asset_bytes.Value().size();
    payload.main_asset_guid = material_guid;
    toc.payloads.push_back(payload);
    auto bundle_bytes = serialize::WriteBundle(toc, asset_bytes.Value());
    REQUIRE(bundle_bytes);
    const std::string bundle_file = bundle_guid.ToHex() + ".bundle";
    WriteFile(package_root / "bundles" / bundle_file, bundle_bytes.Value());

    serialize::ManifestDesc manifest;
    manifest.package_name = "multi";
    manifest.package_version = "1.0.0";
    serialize::ManifestBundleDesc bundle_desc;
    bundle_desc.bundle_guid = bundle_guid;
    bundle_desc.bundle_name = "multi";
    bundle_desc.file_name = bundle_file;
    bundle_desc.file_size = bundle_bytes.Value().size();
    bundle_desc.file_sha256 = core::Sha256(bundle_bytes.Value());
    bundle_desc.file_crc = core::Crc32(bundle_bytes.Value());
    manifest.bundles.push_back(bundle_desc);
    serialize::ManifestAssetDesc asset_desc;
    asset_desc.asset_guid = material_guid;
    asset_desc.address = "multi/asset.prca";
    asset_desc.asset_path = asset_desc.address;
    asset_desc.type_name = "MaterialAsset";
    asset_desc.bundle_id = 0;
    asset_desc.payload_id = 0;
    asset_desc.local_object_id = 0;
    manifest.assets.push_back(asset_desc);
    REQUIRE(serialize::WriteManifestFile(ToUtf8(package_root / "manifest.bytes"), manifest));

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    runtime::ResourcePackageOptions options;
    options.package_name = "multi";
    options.builtin_root = ToUtf8(package_root);
    auto package = system.CreatePackage(options);
    REQUIRE(package);

    auto all = package.Value()->LoadAllAssets("multi/asset.prca");
    INFO(all.LastError().ToString());
    REQUIRE(all.Status() == core::OperationStatus::Succeeded);
    REQUIRE(all.Objects().size() == 2);
    REQUIRE(all.Get<assets::MaterialAsset>() != nullptr);

    auto subs = package.Value()->LoadSubAssetsSync<assets::TextureAsset>("multi/asset.prca");
    REQUIRE(subs.Status() == core::OperationStatus::Succeeded);
    REQUIRE(subs.Objects().size() == 1);
    REQUIRE(subs.Get<assets::TextureAsset>() != nullptr);

    auto filtered = package.Value()->LoadAllAssets<assets::TextureAsset>("multi/asset.prca");
    REQUIRE(filtered.Status() == core::OperationStatus::Succeeded);
    REQUIRE(filtered.Objects().size() == 1);

    all.Release();
    subs.Release();
    filtered.Release();
    REQUIRE(system.DestroyPackage("multi"));
    system.Shutdown();
}

TEST_CASE("runtime loading lock rejects new loads", "[runtime]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    runtime::LoadingLock lock(*package.Value());
    REQUIRE(package.Value()->IsLoadingLocked());
    auto blocked = package.Value()->LoadAsset<assets::MaterialAsset>(fixture.material_path);
    REQUIRE(blocked.Status() == core::OperationStatus::Failed);
    REQUIRE(blocked.LastError().code == core::ErrorCode::Canceled);
    blocked.Release();

    lock.Release();
    REQUIRE_FALSE(package.Value()->IsLoadingLocked());
    auto loaded = package.Value()->LoadAsset<assets::MaterialAsset>(fixture.material_path);
    REQUIRE(loaded.Status() == core::OperationStatus::Succeeded);
    loaded.Release();

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime unload-all completes in-flight async loads", "[runtime]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    auto handle = package.Value()->LoadAssetAsync<assets::MaterialAsset>(fixture.material_path);
    REQUIRE(handle.IsValid());
    (void)package.Value()->UnloadAllAssets();
    REQUIRE_FALSE(handle.IsValid());
    REQUIRE(handle.GetObject() == nullptr);
    handle.Release();
    REQUIRE(package.Value()->Stats().handles == 0);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime callbacks are scoped to their provider", "[runtime]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    int material_callbacks = 0;
    int raw_callbacks = 0;
    auto material = package.Value()->LoadAssetAsync<assets::MaterialAsset>(fixture.material_path);
    auto raw = package.Value()->LoadRawFileAsync(fixture.raw_path);
    material.OnComplete(
        [&material_callbacks](const runtime::AssetHandle&) { ++material_callbacks; });
    raw.OnComplete([&raw_callbacks](const runtime::AssetHandle&) { ++raw_callbacks; });

    Pump(system, material);
    Pump(system, raw);
    REQUIRE(material_callbacks == 1);
    REQUIRE(raw_callbacks == 1);

    material.Release();
    raw.Release();
    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime keeps old providers across a manifest switch", "[runtime]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    auto old_handle = package.Value()->LoadAsset<assets::MaterialAsset>(fixture.material_path);
    REQUIRE(old_handle.Status() == core::OperationStatus::Succeeded);

    auto manifest = package.Value()->Manifest();
    REQUIRE(manifest);
    auto changed = CopyManifest(*manifest);
    const auto changed_asset = std::find_if(changed.assets.begin(), changed.assets.end(),
                                            [&](const serialize::ManifestAssetDesc& asset) {
                                                return asset.asset_guid == fixture.material_guid;
                                            });
    REQUIRE(changed_asset != changed.assets.end());
    changed_asset->address = "switched/material.prca";
    changed_asset->asset_path = changed_asset->address;
    auto changed_bytes = serialize::WriteManifest(changed);
    REQUIRE(changed_bytes);
    REQUIRE(package.Value()->LoadManifestFromBytes(changed_bytes.Value()));

    REQUIRE(package.Value()->IsLocationValid("switched/material.prca"));
    REQUIRE_FALSE(package.Value()->IsLocationValid(fixture.material_path));
    REQUIRE(old_handle.Get<assets::MaterialAsset>() != nullptr);

    auto new_handle = package.Value()->LoadAsset("switched/material.prca");
    REQUIRE(new_handle.Status() == core::OperationStatus::Succeeded);
    REQUIRE(new_handle.Get<assets::MaterialAsset>() != nullptr);

    old_handle.Release();
    new_handle.Release();
    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime package factory owns its scheduler", "[runtime]") {
    RuntimeFixture fixture;
    auto package = runtime::ResourcePackage::Create(fixture.PackageOptions());
    REQUIRE(package);

    auto handle = package.Value()->LoadAssetAsync<assets::MaterialAsset>(fixture.material_path);
    REQUIRE(handle.WaitForCompletion().HasValue());
    REQUIRE(handle.Status() == core::OperationStatus::Succeeded);
    REQUIRE(handle.Get<assets::MaterialAsset>() != nullptr);
    handle.Release();
    package.Value()->Destroy();
}

TEST_CASE("runtime stress: loading lock and unload cycles", "[runtime][stress]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    for (int cycle = 0; cycle < 100; ++cycle) {
        {
            runtime::LoadingLock lock(*package.Value());
            REQUIRE(package.Value()->IsLoadingLocked());
            auto blocked = package.Value()->LoadAsset<assets::MaterialAsset>(fixture.material_path);
            REQUIRE(blocked.Status() == core::OperationStatus::Failed);
            blocked.Release();
        }
        REQUIRE_FALSE(package.Value()->IsLoadingLocked());
        auto handle = package.Value()->LoadAsset<assets::MaterialAsset>(fixture.material_path);
        REQUIRE(handle.Status() == core::OperationStatus::Succeeded);
        handle.Release();
        if ((cycle % 10) == 0) {
            (void)package.Value()->UnloadAllAssets();
        }
    }
    (void)package.Value()->UnloadAllAssets();
    REQUIRE(package.Value()->Stats().handles == 0);
    REQUIRE(package.Value()->Leaks().Empty());
    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime stress: concurrent load and release", "[runtime][stress]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    constexpr int kThreads = 4;
    constexpr int kIterations = 150;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([&, worker]() {
            for (int i = 0; i < kIterations; ++i) {
                runtime::AssetHandle handle;
                if (((i + worker) % 3) == 0) {
                    handle = package.Value()->LoadRawFile(fixture.raw_path);
                } else {
                    handle =
                        package.Value()->LoadAsset<assets::MaterialAsset>(fixture.material_path);
                }
                if (!handle.IsValid() || handle.Status() != core::OperationStatus::Succeeded) {
                    ++failures;
                }
                handle.Release();
                if ((i % 25) == 0) {
                    (void)package.Value()->UnloadUnusedAssets();
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    REQUIRE(failures == 0);
    (void)package.Value()->UnloadAllAssets();
    REQUIRE(package.Value()->Stats().handles == 0);
    REQUIRE(package.Value()->Leaks().Empty());
    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}
TEST_CASE("runtime stress: destroy and recreate packages", "[runtime][stress]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));

    for (int cycle = 0; cycle < 25; ++cycle) {
        auto package = system.CreatePackage(fixture.PackageOptions());
        REQUIRE(package);
        for (int i = 0; i < 20; ++i) {
            auto handle = package.Value()->LoadAsset<assets::MaterialAsset>(fixture.material_path);
            REQUIRE(handle.Status() == core::OperationStatus::Succeeded);
            handle.Release();
        }
        (void)package.Value()->UnloadAllAssets();
        REQUIRE(package.Value()->Stats().handles == 0);
        REQUIRE(package.Value()->Leaks().Empty());
        REQUIRE(system.DestroyPackage("game"));
        REQUIRE(system.GetPackage("game") == nullptr);
    }
    system.Shutdown();
}
TEST_CASE("runtime repeated random-style load and release does not leak", "[runtime]") {
    RuntimeFixture fixture;
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    for (int i = 0; i < 1000; ++i) {
        const bool use_material = (i % 2) == 0;
        auto handle = use_material
                          ? package.Value()->LoadAsset<assets::MaterialAsset>(fixture.material_path)
                          : package.Value()->LoadRawFile(fixture.raw_path);
        REQUIRE(handle.Status() == core::OperationStatus::Succeeded);
        handle.Release();
        if ((i % 64) == 0) {
            (void)package.Value()->UnloadUnusedAssets();
        }
    }
    (void)package.Value()->UnloadUnusedAssets();
    REQUIRE(package.Value()->Stats().handles == 0);
    REQUIRE(package.Value()->Leaks().Empty());

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime unload stays within the frame budget", "[runtime][performance]") {
    constexpr std::size_t kExtraAssets = 1000;
    RuntimeFixture fixture(kExtraAssets);
    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.PackageOptions());
    REQUIRE(package);

    std::vector<runtime::AssetHandle> handles;
    handles.reserve(fixture.bulk_raw_paths.size());
    for (const auto& path : fixture.bulk_raw_paths) {
        auto handle = package.Value()->LoadRawFile(path);
        REQUIRE(handle.IsValid());
        REQUIRE(handle.Status() == core::OperationStatus::Succeeded);
        handles.push_back(std::move(handle));
    }
    for (auto& handle : handles) {
        handle.Release();
    }

    std::size_t removed = 0;
    std::chrono::microseconds slowest{0};
    std::vector<std::chrono::microseconds> samples;
    samples.reserve(256);
    int frames = 0;
    for (; frames < 256; ++frames) {
        const auto start = std::chrono::steady_clock::now();
        removed += package.Value()->UnloadUnusedAssets(std::chrono::milliseconds(4));
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        samples.push_back(elapsed);
        slowest = std::max(slowest, elapsed);
        if (elapsed >= std::chrono::milliseconds(4)) {
            std::printf("slow frame=%d removed=%zu elapsed_us=%lld\n", frames, removed,
                        static_cast<long long>(elapsed.count()));
        }
    }
    std::sort(samples.begin(), samples.end());
    const auto p95 = samples[(samples.size() * 95U) / 100U];
    const auto over_budget = static_cast<std::size_t>(
        std::count_if(samples.begin(), samples.end(), [](std::chrono::microseconds value) {
            return value >= std::chrono::milliseconds(4);
        }));
    std::printf("unload budget stress: handles=%zu removed=%zu frames=%d slowest_us=%lld "
                "p95_us=%lld over_budget=%zu\n",
                handles.size(), removed, frames, static_cast<long long>(slowest.count()),
                static_cast<long long>(p95.count()), over_budget);
    const auto stats = package.Value()->Stats();
    REQUIRE(stats.providers == 0);
    REQUIRE(stats.bundle_loaders == 0);
    REQUIRE(p95 < std::chrono::milliseconds(4));
    REQUIRE(package.Value()->Stats().handles == 0);
    REQUIRE(package.Value()->Leaks().Empty());

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}













