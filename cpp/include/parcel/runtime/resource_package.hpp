#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/core/result.hpp"
#include "parcel/runtime/api.hpp"
#include "parcel/runtime/diagnostics.hpp"
#include "parcel/runtime/download.hpp"
#include "parcel/runtime/file_system.hpp"
#include "parcel/runtime/handle.hpp"
#include "parcel/scene/scene_loader.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::runtime {

struct ResourcePackageImpl;

struct CacheVerifyOptions {
    CacheVerifyLevel level = CacheVerifyLevel::Content;
    std::size_t max_concurrency = 8;
};

struct CacheVerifyReport {
    std::size_t total = 0;
    std::size_t valid = 0;
    std::size_t missing = 0;
    std::size_t invalid = 0;
    std::size_t removed = 0;
    std::size_t failed = 0;
    std::size_t unsupported = 0;
    std::uint64_t valid_bytes = 0;
};

struct ResourcePackageOptions {
    std::string package_name;
    std::string package_version;
    std::string builtin_root;
    std::string sandbox_root;
    std::string custom_root;
    std::string app_build_id;
    InstallCleanupMode install_cleanup_mode = InstallCleanupMode::None;
    std::vector<std::shared_ptr<IFileSystem>> file_systems;
    std::shared_ptr<const assets::TypeRegistry> type_registry;

    int max_concurrent_bundle_loads = 4;
    bool verify_bundle_on_load = true;
    bool auto_unload_bundle_when_unused = false;
    bool write_cache_sidecar = true;
    std::chrono::milliseconds wait_timeout = std::chrono::milliseconds(0);
    std::shared_ptr<IHttpTransport> download_transport;
    std::shared_ptr<IDownloadUrlProvider> download_url_provider;
    std::shared_ptr<IFileSystem> download_file_system;
    DownloadOptions download_options;
};

class ResourcePackage;

class LoadingLock {
  public:
    LoadingLock() noexcept = default;
    explicit LoadingLock(ResourcePackage& package) noexcept;
    ~LoadingLock();

    LoadingLock(const LoadingLock&) = delete;
    LoadingLock& operator=(const LoadingLock&) = delete;
    LoadingLock(LoadingLock&& other) noexcept;
    LoadingLock& operator=(LoadingLock&& other) noexcept;

    void Release() noexcept;
    [[nodiscard]] bool IsLocked() const noexcept { return package_ != nullptr; }

  private:
    ResourcePackage* package_ = nullptr;
};

class ResourcePackage final {
  public:
    explicit ResourcePackage(std::shared_ptr<ResourcePackageImpl> impl);
    ~ResourcePackage();

    [[nodiscard]] static core::Result<std::shared_ptr<ResourcePackage>>
    Create(const ResourcePackageOptions& options);

    ResourcePackage(const ResourcePackage&) = delete;
    ResourcePackage& operator=(const ResourcePackage&) = delete;

    [[nodiscard]] core::Result<void> Initialize(const ResourcePackageOptions& options);
    void Destroy() noexcept;

    [[nodiscard]] std::string_view Name() const noexcept;
    [[nodiscard]] std::string_view Version() const noexcept;
    [[nodiscard]] std::string RequestVersion() const { return std::string(Version()); }
    [[nodiscard]] ManifestPtr Manifest() const noexcept;
    [[nodiscard]] std::shared_ptr<FileSystemHost> Host() const noexcept;

    [[nodiscard]] core::Result<void> ReloadManifest();
    [[nodiscard]] core::Result<void> LoadManifest() { return ReloadManifest(); }
    [[nodiscard]] core::Result<void> PrefetchManifest();
    [[nodiscard]] core::Result<void> Prefetch() { return PrefetchManifest(); }
    [[nodiscard]] core::Result<void> LoadManifestFromBytes(std::span<const std::byte> bytes);
    [[nodiscard]] core::Result<void> LoadManifest(ManifestPtr manifest);
    [[nodiscard]] core::Result<void> EnsureBundle(std::string_view bundle_name);
    [[nodiscard]] core::Result<void> EnsureBundle(const core::AssetGuid& bundle_guid);
    [[nodiscard]] core::Result<void> Ensure(std::string_view bundle_name) {
        return EnsureBundle(bundle_name);
    }

    [[nodiscard]] AssetHandle LoadAsset(std::string_view location,
                                        assets::TypeId asset_type = assets::kInvalidTypeId,
                                        int priority = 0);
    [[nodiscard]] AssetHandle LoadAssetSync(std::string_view location,
                                            assets::TypeId asset_type = assets::kInvalidTypeId,
                                            int priority = 0) {
        return LoadAsset(location, asset_type, priority);
    }
    [[nodiscard]] AssetHandle LoadAssetAsync(std::string_view location,
                                             assets::TypeId asset_type = assets::kInvalidTypeId,
                                             int priority = 0);
    template <typename T>
    [[nodiscard]] AssetHandle LoadAsset(std::string_view location, int priority = 0) {
        return LoadAsset(location, assets::AssetTypeOf<T>::value, priority);
    }
    template <typename T>
    [[nodiscard]] AssetHandle LoadAssetAsync(std::string_view location, int priority = 0) {
        return LoadAssetAsync(location, assets::AssetTypeOf<T>::value, priority);
    }
    template <typename T>
    [[nodiscard]] AssetHandle LoadAssetSync(std::string_view location, int priority = 0) {
        return LoadAssetSync(location, assets::AssetTypeOf<T>::value, priority);
    }

    [[nodiscard]] AssetHandle LoadSubAssets(std::string_view location,
                                            assets::TypeId asset_type = assets::kInvalidTypeId,
                                            int priority = 0);
    [[nodiscard]] AssetHandle LoadSubAssetsAsync(std::string_view location,
                                                 assets::TypeId asset_type = assets::kInvalidTypeId,
                                                 int priority = 0);
    [[nodiscard]] AssetHandle LoadSubAssetsSync(std::string_view location,
                                                assets::TypeId asset_type = assets::kInvalidTypeId,
                                                int priority = 0) {
        return LoadSubAssets(location, asset_type, priority);
    }
    template <typename T>
    [[nodiscard]] AssetHandle LoadSubAssets(std::string_view location, int priority = 0) {
        return LoadSubAssets(location, assets::AssetTypeOf<T>::value, priority);
    }
    template <typename T>
    [[nodiscard]] AssetHandle LoadSubAssetsAsync(std::string_view location, int priority = 0) {
        return LoadSubAssetsAsync(location, assets::AssetTypeOf<T>::value, priority);
    }
    template <typename T>
    [[nodiscard]] AssetHandle LoadSubAssetsSync(std::string_view location, int priority = 0) {
        return LoadSubAssetsSync(location, assets::AssetTypeOf<T>::value, priority);
    }

    [[nodiscard]] AssetHandle LoadAllAssets(std::string_view location,
                                            assets::TypeId asset_type = assets::kInvalidTypeId,
                                            int priority = 0);
    [[nodiscard]] AssetHandle LoadAllAssetsAsync(std::string_view location,
                                                 assets::TypeId asset_type = assets::kInvalidTypeId,
                                                 int priority = 0);
    [[nodiscard]] AssetHandle LoadAllAssetsSync(std::string_view location,
                                                assets::TypeId asset_type = assets::kInvalidTypeId,
                                                int priority = 0) {
        return LoadAllAssets(location, asset_type, priority);
    }
    template <typename T>
    [[nodiscard]] AssetHandle LoadAllAssets(std::string_view location, int priority = 0) {
        return LoadAllAssets(location, assets::AssetTypeOf<T>::value, priority);
    }
    template <typename T>
    [[nodiscard]] AssetHandle LoadAllAssetsAsync(std::string_view location, int priority = 0) {
        return LoadAllAssetsAsync(location, assets::AssetTypeOf<T>::value, priority);
    }
    template <typename T>
    [[nodiscard]] AssetHandle LoadAllAssetsSync(std::string_view location, int priority = 0) {
        return LoadAllAssetsSync(location, assets::AssetTypeOf<T>::value, priority);
    }

    [[nodiscard]] AssetHandle LoadRawFile(std::string_view location, int priority = 0);
    [[nodiscard]] AssetHandle LoadRawFileSync(std::string_view location, int priority = 0) {
        return LoadRawFile(location, priority);
    }
    [[nodiscard]] AssetHandle LoadRawFileAsync(std::string_view location, int priority = 0);
    [[nodiscard]] AssetHandle LoadBundleFile(std::string_view bundle_name);
    [[nodiscard]] AssetHandle LoadBundleFile(const core::AssetGuid& bundle_guid);
    [[nodiscard]] AssetHandle LoadBundleFileAsync(std::string_view bundle_name);
    [[nodiscard]] AssetHandle LoadBundleFileAsync(const core::AssetGuid& bundle_guid);

    [[nodiscard]] AssetHandle LoadScene(std::string_view location, int priority = 0);
    [[nodiscard]] AssetHandle LoadSceneAsync(std::string_view location, int priority = 0);
    [[nodiscard]] core::Result<void> UnloadScene(const AssetHandle& handle);
    void SetSceneLoader(std::shared_ptr<scene::ISceneLoader> loader, void* registry);

    [[nodiscard]] std::vector<AssetInfo> GetAllAssetInfos() const;
    [[nodiscard]] std::vector<AssetInfo> GetAssetInfosByTag(std::string_view tag) const;
    [[nodiscard]] std::optional<AssetInfo> GetAssetInfoByGuid(const core::AssetGuid& guid) const;
    [[nodiscard]] std::optional<AssetInfo> GetAssetInfo(std::string_view location) const;
    [[nodiscard]] bool IsLocationValid(std::string_view location) const;

    [[nodiscard]] std::uint64_t GetDownloadSize() const;
    [[nodiscard]] std::uint64_t GetDownloadSize(std::string_view location) const;
    [[nodiscard]] std::uint64_t GetDownloadSize(std::span<const std::string> locations) const;
    [[nodiscard]] std::uint64_t GetDownloadSizeByTags(std::span<const std::string> tags) const;

    [[nodiscard]] std::size_t UnloadAllAssets();
    [[nodiscard]] std::size_t
    UnloadUnusedAssets(std::chrono::milliseconds budget = std::chrono::milliseconds(0));
    [[nodiscard]] bool TryUnloadUnusedAsset(std::string_view location);

    [[nodiscard]] core::Result<void> ClearCache(const CacheClearOptions& options);
    [[nodiscard]] core::Result<void> VerifyCache();
    [[nodiscard]] core::Result<CacheVerifyReport>
    VerifyCacheFiles(const CacheVerifyOptions& options = {});
    [[nodiscard]] core::Result<CacheStats> QueryCache() const;

    [[nodiscard]] core::Result<std::shared_ptr<ResourceDownloader>>
    CreateDownloader(std::span<const std::string> locations = {},
                     std::span<const std::string> tags = {});
    [[nodiscard]] core::Result<std::shared_ptr<ResourceUnpacker>> CreateUnpacker();
    [[nodiscard]] core::Result<std::shared_ptr<ResourceImporter>> CreateImporter();

    [[nodiscard]] core::Result<void> StartOperation(core::OperationPtr operation);

    [[nodiscard]] RuntimeStats Stats() const;
    [[nodiscard]] LeakReport Leaks() const;
    [[nodiscard]] std::string LeakReportText() const;

    void SetPriority(int priority) noexcept;
    [[nodiscard]] int Priority() const noexcept;
    void SetAutoUnloadBundleWhenUnused(bool value) noexcept;
    [[nodiscard]] bool AutoUnloadBundleWhenUnused() const noexcept;
    [[nodiscard]] bool IsLoadingLocked() const noexcept;

    void AcquireLoadingLock() noexcept;
    void ReleaseLoadingLock() noexcept;

  private:
    friend class LoadingLock;
    std::shared_ptr<ResourcePackageImpl> impl_;
};

} // namespace parcel::runtime





