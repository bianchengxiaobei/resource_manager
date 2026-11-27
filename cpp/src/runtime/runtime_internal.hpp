#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/core/operation.hpp"
#include "parcel/core/operation_scheduler.hpp"
#include "parcel/core/thread_pool.hpp"
#include "parcel/runtime/bundle_loader.hpp"
#include "parcel/runtime/resource_package.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace parcel::runtime {

class ProviderBase;

[[nodiscard]] std::shared_ptr<ResourceDownloader> MakeBundleDownloader(
    std::string package_name, std::string package_version, ManifestPtr manifest,
    std::span<const std::uint32_t> bundle_indices, std::shared_ptr<FileSystemHost> host,
    std::shared_ptr<IFileSystem> download_file_system,
    std::shared_ptr<IHttpTransport> download_transport,
    std::shared_ptr<IDownloadUrlProvider> download_url_provider, DownloadOptions options);

[[nodiscard]] core::Result<std::shared_ptr<ResourceUnpacker>>
MakeBundleUnpacker(std::string package_name, ManifestPtr manifest,
                   std::shared_ptr<FileSystemHost> host, UnpackOptions options);

[[nodiscard]] core::Result<std::shared_ptr<ResourceImporter>>
MakeBundleImporter(std::string package_name, ManifestPtr manifest,
                   std::shared_ptr<FileSystemHost> host, BundleImportOptions options);

class HandleState final : public std::enable_shared_from_this<HandleState> {
  public:
    HandleState(std::shared_ptr<ResourcePackageImpl> package,
                std::shared_ptr<ProviderBase> provider, AssetInfo info, LoadMethod method);
    HandleState(std::shared_ptr<ResourcePackageImpl> package, AssetInfo info,
                core::OperationStatus status, core::Error error,
                std::shared_ptr<std::vector<std::byte>> standalone_data,
                LoadMethod method = LoadMethod::Asset);
    ~HandleState();

    HandleState(const HandleState&) = delete;
    HandleState& operator=(const HandleState&) = delete;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] bool IsReleased() const noexcept;
    [[nodiscard]] core::OperationStatus Status() const noexcept;
    [[nodiscard]] bool IsDone() const noexcept;
    [[nodiscard]] float Progress() const noexcept;
    [[nodiscard]] const core::Error& LastError() const noexcept;
    [[nodiscard]] LoadMethod Method() const noexcept { return method_; }
    [[nodiscard]] const AssetInfo& Info() const noexcept { return info_; }
    [[nodiscard]] assets::AssetObject* Object() const noexcept;
    [[nodiscard]] std::span<const std::shared_ptr<assets::AssetObject>> Objects() const noexcept;
    [[nodiscard]] std::span<const std::byte> Data() const noexcept;
    [[nodiscard]] core::OperationPtr Operation() const noexcept;

    [[nodiscard]] core::Result<void> Wait(std::chrono::milliseconds timeout) const;

    void OnComplete(std::function<void(const AssetHandle&)> callback);
    void OnComplete(std::function<void()> callback);
    void Release() noexcept;
    void Notify();

  private:
    [[nodiscard]] bool IsDoneNoLock() const noexcept;
    void NotifyStandalone();

    std::shared_ptr<ResourcePackageImpl> package_;
    std::shared_ptr<ProviderBase> provider_;
    AssetInfo info_;
    LoadMethod method_ = LoadMethod::Asset;
    core::OperationStatus standalone_status_ = core::OperationStatus::None;
    core::Error standalone_error_{core::ErrorCode::Ok};
    std::shared_ptr<std::vector<std::byte>> standalone_data_;
    mutable std::mutex mutex_;
    bool notified_ = false;
    std::atomic<bool> released_{false};
    std::vector<std::function<void(const AssetHandle&)>> handle_callbacks_;
    std::vector<std::function<void()>> callbacks_;
};

class ProviderBase final : public core::OperationBase,
                           public std::enable_shared_from_this<ProviderBase> {
  public:
    ProviderBase(std::shared_ptr<ResourcePackageImpl> package, ManifestPtr manifest,
                 const serialize::ManifestAssetView* asset, LoadMethod method,
                 assets::TypeId asset_type, int priority,
                 std::shared_ptr<scene::ISceneLoader> scene_loader = nullptr,
                 void* scene_registry = nullptr);
    ~ProviderBase() override;

    ProviderBase(const ProviderBase&) = delete;
    ProviderBase& operator=(const ProviderBase&) = delete;

    void StartAsync();
    void StartSync();

    [[nodiscard]] AssetHandle CreateHandle();
    [[nodiscard]] AssetHandle CreateHandleLocked();
    [[nodiscard]] const AssetInfo& Info() const noexcept { return info_; }
    [[nodiscard]] const std::string& Key() const noexcept { return key_; }
    [[nodiscard]] assets::AssetObject* Object() const noexcept;
    [[nodiscard]] std::span<const std::shared_ptr<assets::AssetObject>> Objects() const noexcept;
    [[nodiscard]] std::span<const std::byte> Data() const noexcept;
    [[nodiscard]] bool IsInvalidated() const noexcept { return invalidated_.load(); }
    [[nodiscard]] int RefCount() const noexcept { return reference_count_.load(); }
    [[nodiscard]] core::Result<void> WaitForCompletion(std::chrono::milliseconds timeout);

    void ReleaseHandle() noexcept;
    void Invalidate() noexcept;

  protected:
    void OnStart() override;
    void OnTick() override;
    void OnCancel() override;

  private:
    [[nodiscard]] core::Result<void> LoadBlocking();
    [[nodiscard]] core::Result<void> LoadSimulate();
    [[nodiscard]] core::Result<void> InstantiateScene();
    void RunWorker();
    void Integrate();
    void NotifyHandles();
    void ReleaseScene() noexcept;
    void ReleaseBundles() noexcept;
    void OnFinished();

    std::shared_ptr<ResourcePackageImpl> package_;
    ManifestPtr manifest_;
    const serialize::ManifestAssetView* asset_ = nullptr;
    AssetInfo info_;
    LoadMethod method_ = LoadMethod::Asset;
    assets::TypeId asset_type_ = assets::kInvalidTypeId;
    std::string key_;
    bool async_ = false;
    std::atomic<int> reference_count_{0};
    std::atomic<bool> invalidated_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> finished_{false};
    mutable std::mutex result_mutex_;
    core::Error result_error_{core::ErrorCode::Ok};
    std::shared_ptr<assets::AssetObject> object_;
    std::vector<std::shared_ptr<assets::AssetObject>> objects_;
    std::shared_ptr<std::vector<std::byte>> raw_data_;
    std::vector<std::shared_ptr<BundleLoader>> bundles_;
    std::vector<std::weak_ptr<HandleState>> handles_;
    std::mutex handles_mutex_;
    std::mutex bundle_mutex_;
    std::shared_ptr<scene::ISceneLoader> scene_loader_;
    void* scene_registry_ = nullptr;
    scene::SceneInstance scene_instance_;
    std::mutex scene_mutex_;
    bool scene_instantiated_ = false;
    bool scene_released_ = false;
    bool bundles_released_ = false;
    bool completion_notified_ = false;
};

struct PendingDownloadState;

struct ResourcePackageImpl : public std::enable_shared_from_this<ResourcePackageImpl> {
    ResourcePackageImpl() = default;
    ~ResourcePackageImpl();

    ResourcePackageImpl(const ResourcePackageImpl&) = delete;
    ResourcePackageImpl& operator=(const ResourcePackageImpl&) = delete;

    [[nodiscard]] core::Result<void> Initialize(const ResourcePackageOptions& package_options,
                                                std::shared_ptr<core::ThreadPool> thread_pool,
                                                core::OperationScheduler* operation_scheduler);
    void Destroy() noexcept;

    [[nodiscard]] AssetHandle Load(std::string_view location, LoadMethod method,
                                   assets::TypeId asset_type, bool async, int priority);
    [[nodiscard]] core::Result<std::shared_ptr<RawFileReader>>
    OpenRawFile(std::string_view location);
    [[nodiscard]] AssetHandle LoadBundleFile(std::string_view location);
    [[nodiscard]] AssetHandle LoadBundleFile(const core::AssetGuid& guid);
    [[nodiscard]] AssetHandle MakeFailedHandle(AssetInfo info, LoadMethod method,
                                               core::Error error) const;
    [[nodiscard]] AssetHandle
    MakeStandaloneHandle(AssetInfo info, LoadMethod method,
                         std::shared_ptr<std::vector<std::byte>> data) const;

    [[nodiscard]] core::Result<const serialize::ManifestAssetView*>
    ResolveAsset(std::string_view location) const;
    [[nodiscard]] core::Result<std::vector<std::byte>>
    ReadSimulateAsset(const core::AssetGuid& guid) const;
    [[nodiscard]] const serialize::ManifestBundleView*
    FindBundle(std::string_view name) const noexcept;
    [[nodiscard]] AssetInfo MakeAssetInfo(const serialize::ManifestAssetView& asset) const;

    [[nodiscard]] std::shared_ptr<BundleLoader> AcquireBundle(const ManifestPtr& active_manifest,
                                                              std::uint32_t bundle_index,
                                                              core::Error& error);
    void ReleaseBundle(const std::shared_ptr<BundleLoader>& bundle) noexcept;
    [[nodiscard]] bool AcquireBundleSlot() noexcept;
    void ReleaseBundleSlot() noexcept;
    void OnProviderCompleted(const std::shared_ptr<ProviderBase>& provider) noexcept;
    [[nodiscard]] core::Result<void> EnsureBundleDownloaded(const ManifestPtr& active_manifest,
                                                            std::uint32_t bundle_index);
    void RemoveUnused() noexcept;

    [[nodiscard]] core::Result<void> WaitProvider(const std::shared_ptr<ProviderBase>& provider,
                                                  std::chrono::milliseconds timeout);

    [[nodiscard]] std::size_t UnloadAll() noexcept;
    [[nodiscard]] std::size_t UnloadUnused(std::chrono::milliseconds budget) noexcept;
    [[nodiscard]] bool TryUnload(std::string_view location) noexcept;

    [[nodiscard]] bool loaded_lock() const noexcept { return loading_lock_count.load() > 0; }
    [[nodiscard]] bool HasSceneLoader() const;
    [[nodiscard]] PlayMode GetPlayMode() const noexcept { return play_mode; }

    [[nodiscard]] RuntimeStats Stats() const;
    [[nodiscard]] LeakReport Leaks() const;

    std::shared_ptr<core::ThreadPool> pool;
    std::shared_ptr<core::ThreadPool> owned_pool;
    core::OperationScheduler* scheduler = nullptr;
    std::unique_ptr<core::OperationScheduler> owned_scheduler;
    ResourcePackageOptions options;
    PlayMode play_mode = PlayMode::Auto;
    std::string name;
    std::string version;
    std::string simulate_manifest_path;
    std::unordered_map<core::AssetGuid, std::string, core::Guid128Hash> simulate_assets;
    std::shared_ptr<FileSystemHost> host;
    ManifestPtr manifest;
    assets::TypeRegistry registry;
    std::shared_ptr<scene::ISceneLoader> scene_loader;
    void* scene_registry = nullptr;

    mutable std::mutex mutex;
    std::mutex bundle_slot_mutex;
    std::condition_variable bundle_slot_condition;
    int bundle_slots_active = 0;
    int bundle_slots_max = 4;
    std::unordered_map<core::AssetGuid, std::shared_ptr<BundleLoader>, core::Guid128Hash> bundles;
    std::unordered_map<std::string, std::weak_ptr<ProviderBase>> providers;
    std::mutex download_mutex;
    std::unordered_map<core::AssetGuid, std::weak_ptr<PendingDownloadState>, core::Guid128Hash>
        download_tasks;
    std::vector<std::weak_ptr<HandleState>> handles;
    std::atomic<bool> initialized{false};
    std::atomic<bool> destroyed{false};
    std::atomic<int> loading_lock_count{0};
    int priority = 0;
    bool auto_unload = false;
    std::uint64_t next_provider_id = 0;
    std::uint64_t generation = 0;
};

struct RawFileReaderImpl {
    ~RawFileReaderImpl();

    std::shared_ptr<ResourcePackageImpl> package;
    std::shared_ptr<BundleLoader> bundle;
    std::shared_ptr<std::vector<std::byte>> memory;
    std::string raw_name;
    std::uint64_t size = 0;
};

} // namespace parcel::runtime
