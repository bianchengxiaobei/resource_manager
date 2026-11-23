#include "parcel/runtime/resource_package.hpp"

#include "parcel/assets/asset_codec.hpp"
#include "parcel/assets/raw_file_asset.hpp"
#include "parcel/assets/scene_asset.hpp"
#include "parcel/assets/serializers.hpp"
#include "parcel/core/path.hpp"
#include "parcel/runtime/bundle_loader.hpp"
#include "runtime_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

namespace parcel::runtime {
struct PendingDownloadState {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    core::Error error{core::ErrorCode::Ok};
};

namespace {

using core::ErrorCode;
using core::MakeError;
using core::Result;

AssetInfo MakeInfo(const serialize::ManifestAssetView& asset) {
    AssetInfo info;
    info.asset_guid = asset.asset_guid;
    info.address.assign(asset.address);
    info.asset_path.assign(asset.asset_path);
    info.type_name.assign(asset.type_name);
    info.tags.reserve(asset.tags.size());
    for (const std::string_view tag : asset.tags) {
        info.tags.emplace_back(tag);
    }
    return info;
}

AssetInfo MakeBundleInfo(const serialize::ManifestBundleView& bundle) {
    AssetInfo info;
    info.asset_guid = bundle.bundle_guid;
    info.address.assign(bundle.bundle_name);
    info.asset_path.assign(bundle.file_name);
    info.type_name = "BundleFile";
    return info;
}

std::string LowerAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool HasTag(const serialize::ManifestAssetView& asset, std::string_view tag) {
    return std::find(asset.tags.begin(), asset.tags.end(), tag) != asset.tags.end();
}

bool IsCacheRelevantTag(std::string_view tag, const std::vector<std::string>& tags) {
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

Result<ManifestPtr> OpenManifestBytes(std::vector<std::byte> bytes) {
    auto storage = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    auto opened = serialize::ManifestView::Open(storage, true);
    if (!opened) {
        return std::move(opened).TakeError();
    }
    return std::make_shared<const serialize::ManifestView>(std::move(opened).Value());
}

class PackageResolver final : public assets::IObjectResolver {
  public:
    PackageResolver(ManifestPtr manifest, const assets::TypeRegistry& registry,
                    const std::unordered_map<std::uint32_t, std::shared_ptr<BundleLoader>>& bundles)
        : manifest_(std::move(manifest)), registry_(registry), bundles_(bundles) {}

    Result<std::shared_ptr<assets::AssetObject>> Resolve(core::AssetGuid guid) override {
        const auto cached = cache_.find(guid);
        if (cached != cache_.end()) {
            return cached->second;
        }
        if (!resolving_.insert(guid).second) {
            return MakeError(ErrorCode::InvalidArgument, "cyclic external asset reference");
        }

        const serialize::ManifestAssetView* asset = manifest_->FindAssetByGuid(guid);
        if (asset == nullptr) {
            resolving_.erase(guid);
            return MakeError(ErrorCode::NotFound, "external asset is not present in the manifest")
                .WithAsset(guid.ToHex());
        }
        const auto bundle = bundles_.find(asset->bundle_index);
        if (bundle == bundles_.end()) {
            resolving_.erase(guid);
            return MakeError(ErrorCode::NotFound, "external asset bundle is not loaded")
                .WithAsset(guid.ToHex());
        }

        auto payload = bundle->second->ReadPayload(asset->payload_id);
        if (!payload) {
            resolving_.erase(guid);
            return std::move(payload).TakeError();
        }
        auto object = assets::DecodeAssetBytes(payload.Value(), registry_, this, true);
        resolving_.erase(guid);
        if (!object) {
            return std::move(object).TakeError();
        }
        cache_.emplace(guid, object.Value());
        return object;
    }

  private:
    ManifestPtr manifest_;
    const assets::TypeRegistry& registry_;
    std::unordered_map<std::uint32_t, std::shared_ptr<BundleLoader>> bundles_;
    std::unordered_map<core::AssetGuid, std::shared_ptr<assets::AssetObject>, core::Guid128Hash>
        cache_;
    std::unordered_set<core::AssetGuid, core::Guid128Hash> resolving_;
};

std::vector<std::uint32_t> CollectBundleIds(const serialize::ManifestView& manifest,
                                            const serialize::ManifestAssetView& asset) {
    std::vector<std::uint32_t> ids;
    std::unordered_set<std::uint32_t> seen;
    auto add = [&](std::uint32_t id) {
        if (id < manifest.Bundles().size() && seen.insert(id).second) {
            ids.push_back(id);
        }
    };
    add(asset.bundle_index);
    for (const std::uint32_t dependency : asset.dependent_bundle_ids) {
        add(dependency);
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto& bundle = manifest.Bundles()[ids[i]];
        for (const std::uint32_t dependency : bundle.dependent_bundle_ids) {
            add(dependency);
        }
    }
    return ids;
}

} // namespace

ProviderBase::ProviderBase(std::shared_ptr<ResourcePackageImpl> package, ManifestPtr manifest,
                           const serialize::ManifestAssetView* asset, LoadMethod method,
                           assets::TypeId asset_type, int priority,
                           std::shared_ptr<scene::ISceneLoader> scene_loader, void* scene_registry)
    : package_(std::move(package)), manifest_(std::move(manifest)), asset_(asset),
      info_(asset != nullptr ? MakeInfo(*asset) : AssetInfo{}), method_(method),
      asset_type_(asset_type), scene_loader_(std::move(scene_loader)),
      scene_registry_(scene_registry) {
    SetPriority(priority);
    key_ = info_.asset_guid.ToHex();
    key_ += ':';
    key_ += ToString(method_);
    key_ += ':';
    key_ += std::to_string(asset_type_);
}

ProviderBase::~ProviderBase() {
    ReleaseScene();
    ReleaseBundles();
}

void ProviderBase::StartAsync() {
    if (Status() != core::OperationStatus::None) {
        return;
    }
    if (started_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    async_ = true;
    if (!package_ || package_->scheduler == nullptr) {
        Fail(MakeError(ErrorCode::Internal, "provider has no scheduler"));
        return;
    }
    package_->scheduler->Add(shared_from_this());
}

void ProviderBase::StartSync() {
    if (Status() != core::OperationStatus::None) {
        return;
    }
    if (started_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    async_ = false;
    Start();
}

AssetHandle ProviderBase::CreateHandle() {
    auto state = std::make_shared<HandleState>(package_, shared_from_this(), info_, method_);
    reference_count_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(handles_mutex_);
        handles_.push_back(state);
    }
    if (package_) {
        std::lock_guard lock(package_->mutex);
        package_->handles.push_back(state);
    }
    return AssetHandle(std::move(state));
}

AssetHandle ProviderBase::CreateHandleLocked() {
    auto state = std::make_shared<HandleState>(package_, shared_from_this(), info_, method_);
    reference_count_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(handles_mutex_);
        handles_.push_back(state);
    }
    if (package_) {
        package_->handles.push_back(state);
    }
    return AssetHandle(std::move(state));
}

assets::AssetObject* ProviderBase::Object() const noexcept {
    if (invalidated_.load()) {
        return nullptr;
    }
    std::lock_guard lock(result_mutex_);
    return object_.get();
}

std::span<const std::shared_ptr<assets::AssetObject>> ProviderBase::Objects() const noexcept {
    static const std::vector<std::shared_ptr<assets::AssetObject>> empty;
    if (invalidated_.load()) {
        return empty;
    }
    std::lock_guard lock(result_mutex_);
    return objects_;
}

std::span<const std::byte> ProviderBase::Data() const noexcept {
    static const std::vector<std::byte> empty;
    if (invalidated_.load()) {
        return empty;
    }
    std::lock_guard lock(result_mutex_);
    return raw_data_ ? std::span<const std::byte>(*raw_data_) : std::span<const std::byte>(empty);
}

Result<void> ProviderBase::WaitForCompletion(std::chrono::milliseconds timeout) {
    if (IsDone()) {
        if (Status() == core::OperationStatus::Failed) {
            return LastError();
        }
        if (Status() == core::OperationStatus::Canceled) {
            return MakeError(ErrorCode::Canceled, "asset operation was canceled");
        }
        return {};
    }
    if (!package_ || package_->scheduler == nullptr) {
        return MakeError(ErrorCode::Internal, "provider has no scheduler");
    }
    return core::OperationScheduler::WaitForCompletion(*package_->scheduler, shared_from_this(),
                                                       timeout);
}

void ProviderBase::ReleaseHandle() noexcept {
    const int previous = reference_count_.fetch_sub(1, std::memory_order_acq_rel);
    if (previous <= 1) {
        reference_count_.store(0, std::memory_order_release);
        if (finished_.load(std::memory_order_acquire) && IsDone()) {
            ReleaseScene();
            ReleaseBundles();
        }
    }
}

void ProviderBase::Invalidate() noexcept {
    invalidated_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(result_mutex_);
        object_.reset();
        objects_.clear();
        raw_data_.reset();
    }
    ReleaseScene();
    if (finished_.load(std::memory_order_acquire) && IsDone()) {
        ReleaseBundles();
    }
}

void ProviderBase::OnStart() {
    if (!package_ || asset_ == nullptr || manifest_ == nullptr) {
        Fail(MakeError(ErrorCode::InvalidArgument, "provider is not initialized"));
        return;
    }
    SetProgress(0.0f);
    if (!async_) {
        auto result = LoadBlocking();
        {
            std::lock_guard lock(result_mutex_);
            result_error_ = result ? core::Error{ErrorCode::Ok} : result.GetError();
        }
        finished_.store(true, std::memory_order_release);
        Integrate();
        return;
    }
    if (!package_->pool) {
        Fail(MakeError(ErrorCode::Internal, "provider has no worker pool"));
        return;
    }
    auto self = shared_from_this();
    if (!package_->pool->Post([self] { self->RunWorker(); })) {
        Fail(MakeError(ErrorCode::Internal, "failed to post provider worker"));
    }
}

void ProviderBase::OnTick() {
    if (finished_.load(std::memory_order_acquire)) {
        Integrate();
    } else {
        SetProgress(0.25f);
    }
}

void ProviderBase::OnCancel() { invalidated_.store(true, std::memory_order_release); }

void ProviderBase::RunWorker() {
    auto result = LoadBlocking();
    {
        std::lock_guard lock(result_mutex_);
        result_error_ = result ? core::Error{ErrorCode::Ok} : result.GetError();
    }
    finished_.store(true, std::memory_order_release);
    if (IsCancellationRequested() || invalidated_.load()) {
        ReleaseBundles();
    }
}

void ProviderBase::Integrate() {
    if (IsCancellationRequested() || invalidated_.load()) {
        if (!IsDone()) {
            Fail(MakeError(ErrorCode::Canceled, "asset provider was invalidated"));
        }
        NotifyHandles();
        ReleaseScene();
        ReleaseBundles();
        return;
    }

    core::Error error;
    {
        std::lock_guard lock(result_mutex_);
        error = result_error_;
    }
    if (error.IsOk()) {
        auto instantiated = InstantiateScene();
        if (!instantiated) {
            error = instantiated.GetError();
            std::lock_guard lock(result_mutex_);
            object_.reset();
            objects_.clear();
        }
    }
    if (!error.IsOk()) {
        if (error.package.empty() && package_) {
            error.WithPackage(package_->name);
        }
        if (error.asset.empty()) {
            error.WithAsset(info_.asset_guid.ToHex());
        }
        Fail(error);
        ReleaseBundles();
    } else {
        Succeed();
    }
    NotifyHandles();
    OnFinished();
}

Result<void> ProviderBase::InstantiateScene() {
    if (method_ != LoadMethod::Scene) {
        return {};
    }
    if (!scene_loader_) {
        return MakeError(ErrorCode::Unsupported, "no scene loader is configured");
    }
    {
        std::lock_guard lock(scene_mutex_);
        if (scene_instantiated_) {
            return {};
        }
        if (scene_released_) {
            return MakeError(ErrorCode::Canceled, "scene provider was released");
        }
    }

    std::shared_ptr<assets::AssetObject> object;
    {
        std::lock_guard lock(result_mutex_);
        object = object_;
    }
    if (!object || object->Type() != assets::kSceneAssetType) {
        return MakeError(ErrorCode::InvalidArgument, "requested asset is not a scene");
    }

    auto scene = std::static_pointer_cast<assets::SceneAsset>(object);
    auto instantiated = scene_loader_->Instantiate(scene->scene, scene_registry_);
    if (!instantiated) {
        return std::move(instantiated).TakeError();
    }

    scene::SceneInstance instance = std::move(instantiated).Value();
    {
        std::lock_guard lock(scene_mutex_);
        if (!scene_released_ && !invalidated_.load(std::memory_order_acquire)) {
            scene_instance_ = std::move(instance);
            scene_instantiated_ = true;
            return {};
        }
    }
    (void)scene_loader_->Destroy(instance);
    return MakeError(ErrorCode::Canceled, "scene load was canceled during instantiation");
}

void ProviderBase::ReleaseScene() noexcept {
    std::lock_guard lock(scene_mutex_);
    scene_released_ = true;
    if (!scene_instantiated_) {
        return;
    }
    scene_instantiated_ = false;
    if (scene_loader_) {
        (void)scene_loader_->Destroy(scene_instance_);
    }
    scene_instance_ = {};
}

Result<void> ProviderBase::LoadBlocking() {
    if (!package_ || !manifest_ || asset_ == nullptr) {
        return MakeError(ErrorCode::InvalidArgument, "asset provider has no manifest");
    }
    if (asset_->bundle_index >= manifest_->Bundles().size()) {
        return MakeError(ErrorCode::Corrupt, "asset bundle index is out of range")
            .WithAsset(info_.asset_guid.ToHex());
    }

    const std::vector<std::uint32_t> bundle_ids = CollectBundleIds(*manifest_, *asset_);
    std::unordered_map<std::uint32_t, std::shared_ptr<BundleLoader>> bundle_map;
    bundles_.clear();
    bundles_.reserve(bundle_ids.size());
    for (const std::uint32_t bundle_id : bundle_ids) {
        if (IsCancellationRequested() || invalidated_.load()) {
            return MakeError(ErrorCode::Canceled, "asset load was canceled");
        }
        core::Error error;
        auto loader = package_->AcquireBundle(manifest_, bundle_id, error);
        if (!loader) {
            return error;
        }
        bundles_.push_back(loader);
        bundle_map.emplace(bundle_id, loader);
    }

    for (const auto& loader : bundles_) {
        if (IsCancellationRequested() || invalidated_.load()) {
            return MakeError(ErrorCode::Canceled, "asset load was canceled");
        }
        auto downloadable = package_->EnsureBundleDownloaded(manifest_, loader->BundleIndex());
        if (!downloadable) {
            return std::move(downloadable).TakeError();
        }
        if (!package_->AcquireBundleSlot()) {
            return MakeError(ErrorCode::Canceled, "package stopped while loading bundles");
        }
        auto loaded = loader->Load();
        package_->ReleaseBundleSlot();
        if (!loaded) {
            core::Error load_error = std::move(loaded).TakeError();
            const bool recoverable = load_error.code == ErrorCode::HashMismatch ||
                                     load_error.code == ErrorCode::Corrupt ||
                                     load_error.code == ErrorCode::NotFound;
            if (recoverable) {
                auto repaired = package_->EnsureBundleDownloaded(manifest_, loader->BundleIndex());
                if (repaired) {
                    auto retried = loader->Load();
                    if (retried) {
                        continue;
                    }
                }
            }
            return load_error;
        }
    }

    const auto main = bundle_map.find(asset_->bundle_index);
    if (main == bundle_map.end()) {
        return MakeError(ErrorCode::Internal, "main bundle was not acquired")
            .WithAsset(info_.asset_guid.ToHex());
    }

    PackageResolver resolver(manifest_, package_->registry, bundle_map);
    BundleHandle handle(main->second, &package_->registry, &resolver, asset_->payload_id,
                        asset_->local_object_id);

    if (method_ == LoadMethod::AllAssets || method_ == LoadMethod::SubAssets) {
        auto result =
            method_ == LoadMethod::AllAssets ? handle.LoadAllAssets() : handle.LoadSubAssets();
        if (!result) {
            return std::move(result).TakeError();
        }
        std::vector<std::shared_ptr<assets::AssetObject>> objects;
        objects.reserve(result.Value().size());
        for (auto& object : result.Value()) {
            if (asset_type_ != assets::kInvalidTypeId && object->Type() != asset_type_) {
                continue;
            }
            objects.push_back(std::move(object));
        }
        std::lock_guard lock(result_mutex_);
        if (!objects.empty()) {
            object_ = objects.front();
        }
        objects_ = std::move(objects);
        return {};
    }

    auto object = handle.LoadAsset();
    if (!object) {
        return std::move(object).TakeError();
    }
    if (asset_type_ != assets::kInvalidTypeId && object.Value()->Type() != asset_type_) {
        return MakeError(ErrorCode::InvalidArgument, "asset type does not match the request")
            .WithAsset(info_.asset_guid.ToHex());
    }

    if (method_ == LoadMethod::RawFile) {
        if (object.Value()->Type() != assets::kRawFileAssetType) {
            return MakeError(ErrorCode::InvalidArgument, "requested asset is not a raw file")
                .WithAsset(info_.asset_guid.ToHex());
        }
        auto raw = std::static_pointer_cast<assets::RawFileAsset>(object.Value());
        {
            std::lock_guard lock(result_mutex_);
            object_ = object.Value();
            objects_.assign(1, object.Value());
            raw_data_ = std::make_shared<std::vector<std::byte>>(raw->data);
        }
        return {};
    }

    {
        std::lock_guard lock(result_mutex_);
        object_ = object.Value();
        objects_.assign(1, object.Value());
    }
    return {};
}

void ProviderBase::NotifyHandles() {
    if (completion_notified_) {
        return;
    }
    completion_notified_ = true;
    std::vector<std::shared_ptr<HandleState>> handles;
    {
        std::lock_guard lock(handles_mutex_);
        for (auto it = handles_.begin(); it != handles_.end();) {
            auto handle = it->lock();
            if (!handle) {
                it = handles_.erase(it);
                continue;
            }
            handles.push_back(std::move(handle));
            ++it;
        }
    }
    for (const auto& handle : handles) {
        if (handle) {
            handle->Notify();
        }
    }
}

void ProviderBase::ReleaseBundles() noexcept {
    std::lock_guard lock(bundle_mutex_);
    if (bundles_released_) {
        return;
    }
    bundles_released_ = true;
    if (!package_) {
        return;
    }
    for (const auto& bundle : bundles_) {
        if (bundle) {
            package_->ReleaseBundle(bundle);
        }
    }
    bundles_.clear();
}

void ProviderBase::OnFinished() {
    if (!package_) {
        return;
    }
    package_->OnProviderCompleted(shared_from_this());
    if (reference_count_.load(std::memory_order_acquire) <= 0) {
        ReleaseScene();
        ReleaseBundles();
        if (package_->auto_unload) {
            package_->RemoveUnused();
        }
    }
}

ResourcePackageImpl::~ResourcePackageImpl() { Destroy(); }

bool ResourcePackageImpl::HasSceneLoader() const {
    std::lock_guard lock(mutex);
    return static_cast<bool>(scene_loader);
}

Result<void> ResourcePackageImpl::Initialize(const ResourcePackageOptions& package_options,
                                             std::shared_ptr<core::ThreadPool> thread_pool,
                                             core::OperationScheduler* operation_scheduler) {
    if (initialized && !destroyed) {
        return MakeError(ErrorCode::InvalidArgument, "resource package is already initialized");
    }
    options = package_options;
    owned_pool.reset();
    owned_scheduler.reset();
    pool = std::move(thread_pool);
    if (!pool) {
        core::ThreadPoolOptions pool_options;
        pool_options.thread_count = 0;
        pool_options.thread_name_prefix = "parcel-package";
        owned_pool = std::make_shared<core::ThreadPool>(pool_options);
        pool = owned_pool;
    }
    scheduler = operation_scheduler;
    if (scheduler == nullptr) {
        owned_scheduler =
            std::make_unique<core::OperationScheduler>(nullptr, std::chrono::milliseconds(4));
        scheduler = owned_scheduler.get();
    }
    auto_unload = options.auto_unload_bundle_when_unused;
    priority = 0;
    destroyed = false;
    initialized = false;
    generation = 0;
    bundle_slots_max =
        options.max_concurrent_bundle_loads > 0 ? options.max_concurrent_bundle_loads : 1;

    if (options.type_registry) {
        registry = *options.type_registry;
    } else {
        PARCEL_TRY(assets::RegisterStandardAssetSerializers(registry));
    }

    auto active_host = std::make_shared<FileSystemHost>(options.package_name);
    if (!options.file_systems.empty()) {
        for (const auto& file_system : options.file_systems) {
            active_host->AddFileSystem(file_system);
        }
    } else {
        if (!options.builtin_root.empty()) {
            FileSystemOptions filesystem_options;
            filesystem_options.package_name = options.package_name;
            filesystem_options.root = options.builtin_root;
            filesystem_options.read_only = true;
            filesystem_options.verify = options.verify_bundle_on_load;
            active_host->AddFileSystem(
                std::make_shared<BuiltinFileSystem>(std::move(filesystem_options)));
        }
        if (!options.sandbox_root.empty()) {
            FileSystemOptions filesystem_options;
            filesystem_options.package_name = options.package_name;
            filesystem_options.root = options.sandbox_root;
            filesystem_options.read_only = false;
            filesystem_options.verify = options.verify_bundle_on_load;
            filesystem_options.write_sidecar = options.write_cache_sidecar;
            filesystem_options.app_build_id = options.app_build_id;
            filesystem_options.install_cleanup_mode = options.install_cleanup_mode;
            active_host->AddFileSystem(
                std::make_shared<SandboxFileSystem>(std::move(filesystem_options)));
        }
        if (!options.custom_root.empty()) {
            FileSystemOptions filesystem_options;
            filesystem_options.package_name = options.package_name;
            filesystem_options.root = options.custom_root;
            filesystem_options.read_only = false;
            filesystem_options.verify = options.verify_bundle_on_load;
            filesystem_options.write_sidecar = options.write_cache_sidecar;
            filesystem_options.app_build_id = options.app_build_id;
            filesystem_options.install_cleanup_mode = options.install_cleanup_mode;
            active_host->AddFileSystem(
                std::make_shared<CustomFileSystem>(std::move(filesystem_options)));
        }
    }

    auto initialized_host = active_host->Initialize();
    if (!initialized_host) {
        return std::move(initialized_host).TakeError();
    }
    auto loaded_manifest = active_host->LoadManifest();
    if (!loaded_manifest) {
        active_host->Shutdown();
        return std::move(loaded_manifest).TakeError();
    }

    host = std::move(active_host);
    manifest = std::move(loaded_manifest).Value();
    name =
        options.package_name.empty() ? std::string(manifest->PackageName()) : options.package_name;
    version = std::string(manifest->PackageVersion());
    if (!options.package_version.empty()) {
        version = options.package_version;
    }
    initialized = true;
    return {};
}

void ResourcePackageImpl::Destroy() noexcept {
    if (destroyed) {
        return;
    }
    {
        std::lock_guard slot_lock(bundle_slot_mutex);
        destroyed = true;
    }
    initialized = false;
    bundle_slot_condition.notify_all();
    std::vector<std::shared_ptr<ProviderBase>> active;
    std::vector<std::shared_ptr<HandleState>> handle_states;
    {
        std::lock_guard lock(mutex);
        for (auto& entry : providers) {
            if (auto provider = entry.second.lock()) {
                active.push_back(std::move(provider));
            }
        }
        providers.clear();
        bundles.clear();
        for (auto& entry : handles) {
            if (auto handle = entry.lock()) {
                handle_states.push_back(std::move(handle));
            }
        }
        handles.clear();
    }
    for (auto& handle : handle_states) {
        handle->Release();
    }
    for (auto& provider : active) {
        provider->Invalidate();
    }
    if (scheduler != nullptr && !scheduler->IsUpdating()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            bool pending = false;
            for (const auto& provider : active) {
                if (provider && !provider->IsDone()) {
                    pending = true;
                    break;
                }
            }
            if (!pending || std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            (void)scheduler->Update();
        }
    }
    if (host) {
        host->Shutdown();
        host.reset();
    }
    if (owned_pool) {
        owned_pool->Stop(true);
        owned_pool.reset();
    }
    if (owned_scheduler) {
        owned_scheduler.reset();
    }
    scheduler = nullptr;
    manifest.reset();
}

AssetHandle ResourcePackageImpl::MakeFailedHandle(AssetInfo info, LoadMethod method,
                                                  core::Error error) const {
    auto self = const_cast<ResourcePackageImpl*>(this)->shared_from_this();
    auto state = std::make_shared<HandleState>(std::move(self), std::move(info),
                                               core::OperationStatus::Failed, std::move(error),
                                               nullptr, method);
    return AssetHandle(std::move(state));
}

AssetHandle
ResourcePackageImpl::MakeStandaloneHandle(AssetInfo info, LoadMethod method,
                                          std::shared_ptr<std::vector<std::byte>> data) const {
    auto self = const_cast<ResourcePackageImpl*>(this)->shared_from_this();
    auto state = std::make_shared<HandleState>(std::move(self), std::move(info),
                                               core::OperationStatus::Succeeded,
                                               core::Error{ErrorCode::Ok}, std::move(data), method);
    return AssetHandle(std::move(state));
}

Result<const serialize::ManifestAssetView*>
ResourcePackageImpl::ResolveAsset(std::string_view location) const {
    if (!manifest) {
        return MakeError(ErrorCode::Internal, "package manifest is not loaded");
    }
    const auto* asset = manifest->FindAssetByAddress(location);
    if (asset == nullptr) {
        asset = manifest->FindAssetByPath(location);
    }
    if (asset == nullptr) {
        const std::string text(location);
        auto guid = core::AssetGuid::FromHex(text);
        if (guid) {
            asset = manifest->FindAssetByGuid(guid.Value());
        }
    }
    if (asset == nullptr &&
        (manifest->Header().flags & serialize::kManifestFlagLocationToLower) != 0) {
        const std::string lower = LowerAscii(location);
        for (const auto& candidate : manifest->Assets()) {
            if (LowerAscii(candidate.address) == lower ||
                LowerAscii(candidate.asset_path) == lower) {
                asset = &candidate;
                break;
            }
        }
    }
    if (asset == nullptr &&
        (manifest->Header().flags & serialize::kManifestFlagSupportExtensionless) != 0) {
        const std::string text(location);
        const std::string with_extension = text + ".prca";
        asset = manifest->FindAssetByAddress(with_extension);
        if (asset == nullptr) {
            asset = manifest->FindAssetByPath(with_extension);
        }
    }
    if (asset == nullptr) {
        return MakeError(ErrorCode::NotFound, "asset location was not found in the manifest")
            .WithPackage(name)
            .WithAsset(std::string(location));
    }
    return asset;
}

const serialize::ManifestBundleView*
ResourcePackageImpl::FindBundle(std::string_view bundle_name) const noexcept {
    if (!manifest) {
        return nullptr;
    }
    const auto* bundle = manifest->FindBundleByName(bundle_name);
    if (bundle == nullptr) {
        bundle = manifest->FindBundleByFileName(bundle_name);
    }
    if (bundle == nullptr) {
        bundle = manifest->FindBundleByFileName(core::path::FileName(bundle_name));
    }
    if (bundle == nullptr) {
        auto guid = core::AssetGuid::FromHex(std::string(bundle_name));
        if (guid) {
            bundle = manifest->FindBundleByGuid(guid.Value());
        }
    }
    return bundle;
}

AssetInfo ResourcePackageImpl::MakeAssetInfo(const serialize::ManifestAssetView& asset) const {
    return MakeInfo(asset);
}

std::shared_ptr<BundleLoader> ResourcePackageImpl::AcquireBundle(const ManifestPtr& active_manifest,
                                                                 std::uint32_t bundle_index,
                                                                 core::Error& error) {
    if (!active_manifest || !host || bundle_index >= active_manifest->Bundles().size()) {
        error = MakeError(ErrorCode::Corrupt, "bundle index is out of range").WithPackage(name);
        return nullptr;
    }
    const auto& view = active_manifest->Bundles()[bundle_index];
    std::lock_guard lock(mutex);
    if (destroyed || loaded_lock()) {
        error = MakeError(ErrorCode::Canceled, "package is not accepting loads")
                    .WithPackage(name)
                    .WithBundle(std::string(view.bundle_name));
        return nullptr;
    }
    const auto found = bundles.find(view.bundle_guid);
    if (found != bundles.end()) {
        found->second->AddReference();
        return found->second;
    }
    auto loader = std::make_shared<BundleLoader>(host, active_manifest, bundle_index,
                                                 options.verify_bundle_on_load);
    loader->AddReference();
    bundles.emplace(view.bundle_guid, loader);
    return loader;
}

Result<void> ResourcePackageImpl::EnsureBundleDownloaded(const ManifestPtr& active_manifest,
                                                         std::uint32_t bundle_index) {
    if (!active_manifest || !host || bundle_index >= active_manifest->Bundles().size()) {
        return MakeError(ErrorCode::Corrupt, "bundle index is out of range").WithPackage(name);
    }
    const auto& bundle = active_manifest->Bundles()[bundle_index];
    if (!host->IsDownloadRequired(bundle)) {
        return {};
    }
    if (!options.download_transport || !options.download_url_provider) {
        return MakeError(ErrorCode::Unsupported,
                         "bundle is missing and on-demand downloading is not configured")
            .WithPackage(name)
            .WithBundle(std::string(bundle.bundle_name));
    }

    std::shared_ptr<IFileSystem> download_file_system = options.download_file_system;
    if (!download_file_system) {
        for (const auto& file_system : host->FileSystems()) {
            if (file_system && file_system->SupportsBundleDownload()) {
                download_file_system = file_system;
                break;
            }
        }
    }
    if (!download_file_system) {
        return MakeError(ErrorCode::Unsupported, "no file system supports downloading bundles")
            .WithPackage(name)
            .WithBundle(std::string(bundle.bundle_name));
    }

    std::shared_ptr<PendingDownloadState> pending;
    bool owner = false;
    {
        std::lock_guard lock(download_mutex);
        auto found = download_tasks.find(bundle.bundle_guid);
        if (found != download_tasks.end()) {
            pending = found->second.lock();
        }
        if (!pending) {
            pending = std::make_shared<PendingDownloadState>();
            download_tasks[bundle.bundle_guid] = pending;
            owner = true;
        }
    }

    if (!owner) {
        std::unique_lock lock(pending->mutex);
        pending->condition.wait(lock, [&pending] { return pending->done; });
        return pending->error.IsOk() ? Result<void>{} : Result<void>{pending->error};
    }

    const std::vector<std::uint32_t> indices{bundle_index};
    auto downloader =
        MakeBundleDownloader(name, std::string(active_manifest->PackageVersion()), active_manifest,
                             indices, host, download_file_system, options.download_transport,
                             options.download_url_provider, options.download_options);
    downloader->Start();
    while (!downloader->IsDone()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    core::Error error = downloader->LastError();
    if (downloader->Status() != core::OperationStatus::Succeeded && error.IsOk()) {
        error = MakeError(ErrorCode::Internal, "bundle download did not succeed")
                    .WithPackage(name)
                    .WithBundle(std::string(bundle.bundle_name));
    }
    {
        std::lock_guard lock(pending->mutex);
        pending->error = error;
        pending->done = true;
    }
    pending->condition.notify_all();
    {
        std::lock_guard lock(download_mutex);
        auto found = download_tasks.find(bundle.bundle_guid);
        if (found != download_tasks.end()) {
            download_tasks.erase(found);
        }
    }
    return error.IsOk() ? Result<void>{} : Result<void>{error};
}

void ResourcePackageImpl::ReleaseBundle(const std::shared_ptr<BundleLoader>& bundle) noexcept {
    if (!bundle) {
        return;
    }
    const bool unused = bundle->ReleaseReference();
    if (!unused || !auto_unload) {
        return;
    }
    std::lock_guard lock(mutex);
    for (auto it = bundles.begin(); it != bundles.end(); ++it) {
        if (it->second == bundle) {
            it->second->Unload();
            bundles.erase(it);
            break;
        }
    }
}

bool ResourcePackageImpl::AcquireBundleSlot() noexcept {
    std::unique_lock lock(bundle_slot_mutex);
    bundle_slot_condition.wait(lock, [this] {
        return destroyed || loaded_lock() || bundle_slots_active < bundle_slots_max;
    });
    if (destroyed || loaded_lock()) {
        return false;
    }
    ++bundle_slots_active;
    return true;
}

void ResourcePackageImpl::ReleaseBundleSlot() noexcept {
    std::lock_guard lock(bundle_slot_mutex);
    if (bundle_slots_active > 0) {
        --bundle_slots_active;
    }
    bundle_slot_condition.notify_one();
}

void ResourcePackageImpl::OnProviderCompleted(
    const std::shared_ptr<ProviderBase>& provider) noexcept {
    (void)provider;
}

Result<void> ResourcePackageImpl::WaitProvider(const std::shared_ptr<ProviderBase>& provider,
                                               std::chrono::milliseconds timeout) {
    if (!provider) {
        return MakeError(ErrorCode::InvalidArgument, "provider is null");
    }
    if (provider->IsDone()) {
        if (provider->Status() == core::OperationStatus::Failed) {
            return provider->LastError();
        }
        if (provider->Status() == core::OperationStatus::Canceled) {
            return MakeError(ErrorCode::Canceled, "asset operation was canceled");
        }
        return {};
    }
    if (scheduler == nullptr) {
        return MakeError(ErrorCode::Internal, "package has no operation scheduler");
    }
    return core::OperationScheduler::WaitForCompletion(*scheduler, provider, timeout);
}

AssetHandle ResourcePackageImpl::Load(std::string_view location, LoadMethod method,
                                      assets::TypeId asset_type, bool async, int load_priority) {
    if (!initialized || destroyed) {
        return MakeFailedHandle(
            AssetInfo{}, method,
            MakeError(ErrorCode::InvalidArgument, "resource package is not initialized")
                .WithPackage(name));
    }
    if (loaded_lock()) {
        return MakeFailedHandle(
            AssetInfo{}, method,
            MakeError(ErrorCode::Canceled, "resource package is being unloaded").WithPackage(name));
    }

    auto resolved = ResolveAsset(location);
    if (!resolved) {
        return MakeFailedHandle(AssetInfo{}, method, std::move(resolved).TakeError());
    }
    const auto& asset = *resolved.Value();
    AssetInfo info = MakeAssetInfo(asset);

    std::shared_ptr<ProviderBase> provider;
    AssetHandle handle;
    bool created = false;
    {
        std::lock_guard lock(mutex);
        if (loaded_lock()) {
            return MakeFailedHandle(
                std::move(info), method,
                MakeError(ErrorCode::Canceled, "resource package is being unloaded")
                    .WithPackage(name));
        }
        std::shared_ptr<scene::ISceneLoader> loader;
        void* loader_registry = nullptr;
        if (method == LoadMethod::Scene) {
            loader = scene_loader;
            loader_registry = scene_registry;
            if (!loader) {
                return MakeFailedHandle(
                    std::move(info), method,
                    MakeError(ErrorCode::Unsupported, "no scene loader is configured")
                        .WithPackage(name));
            }
        }
        const std::string key = std::to_string(generation) + ":" + info.asset_guid.ToHex() + ":" +
                                std::string(ToString(method)) + ":" + std::to_string(asset_type);
        auto found = providers.find(key);
        if (found != providers.end()) {
            provider = found->second.lock();
        }
        if (!provider || provider->IsInvalidated() ||
            provider->Status() == core::OperationStatus::Failed ||
            provider->Status() == core::OperationStatus::Canceled) {
            provider = std::make_shared<ProviderBase>(shared_from_this(), manifest, &asset, method,
                                                      asset_type, load_priority + this->priority,
                                                      std::move(loader), loader_registry);
            providers[key] = provider;
            created = true;
        }
        handle = provider->CreateHandleLocked();
    }

    if (!provider || !handle.IsValid()) {
        return MakeFailedHandle(
            std::move(info), method,
            MakeError(ErrorCode::Internal, "failed to create provider").WithPackage(name));
    }
    if (created) {
        if (async) {
            provider->StartAsync();
        } else {
            provider->StartSync();
            if (!provider->IsDone()) {
                (void)WaitProvider(provider, options.wait_timeout);
            }
        }
    } else if (!async && !provider->IsDone()) {
        (void)WaitProvider(provider, options.wait_timeout);
    } else if (provider->Status() == core::OperationStatus::None) {
        if (async) {
            provider->StartAsync();
        } else {
            provider->StartSync();
        }
    }
    return handle;
}

AssetHandle ResourcePackageImpl::LoadBundleFile(std::string_view location) {
    if (!initialized || destroyed || loaded_lock()) {
        return MakeFailedHandle(
            AssetInfo{}, LoadMethod::BundleFile,
            MakeError(ErrorCode::InvalidArgument, "resource package is not initialized")
                .WithPackage(name));
    }
    const auto* bundle = FindBundle(location);
    if (bundle == nullptr) {
        return MakeFailedHandle(AssetInfo{}, LoadMethod::BundleFile,
                                MakeError(ErrorCode::NotFound, "bundle was not found")
                                    .WithPackage(name)
                                    .WithBundle(std::string(location)));
    }
    const auto index = static_cast<std::uint32_t>(bundle - manifest->Bundles().data());
    auto downloadable = EnsureBundleDownloaded(manifest, index);
    if (!downloadable) {
        return MakeFailedHandle(MakeBundleInfo(*bundle), LoadMethod::BundleFile,
                                std::move(downloadable).TakeError());
    }
    auto bytes = host->LoadBundle(*bundle);
    if (!bytes) {
        core::Error load_error = std::move(bytes).TakeError();
        if (load_error.code == ErrorCode::HashMismatch || load_error.code == ErrorCode::Corrupt ||
            load_error.code == ErrorCode::NotFound) {
            auto repaired = EnsureBundleDownloaded(manifest, index);
            if (repaired) {
                bytes = host->LoadBundle(*bundle);
                if (bytes) {
                    auto data = std::make_shared<std::vector<std::byte>>(std::move(bytes).Value());
                    return MakeStandaloneHandle(MakeBundleInfo(*bundle), LoadMethod::BundleFile,
                                                std::move(data));
                }
            }
        }
        return MakeFailedHandle(MakeBundleInfo(*bundle), LoadMethod::BundleFile,
                                std::move(load_error));
    }
    auto data = std::make_shared<std::vector<std::byte>>(std::move(bytes).Value());
    return MakeStandaloneHandle(MakeBundleInfo(*bundle), LoadMethod::BundleFile, std::move(data));
}

AssetHandle ResourcePackageImpl::LoadBundleFile(const core::AssetGuid& guid) {
    if (!manifest) {
        return MakeFailedHandle(
            AssetInfo{}, LoadMethod::BundleFile,
            MakeError(ErrorCode::InvalidArgument, "no manifest").WithPackage(name));
    }
    const auto* bundle = manifest->FindBundleByGuid(guid);
    if (bundle == nullptr) {
        return MakeFailedHandle(AssetInfo{}, LoadMethod::BundleFile,
                                MakeError(ErrorCode::NotFound, "bundle GUID was not found")
                                    .WithPackage(name)
                                    .WithBundle(guid.ToHex()));
    }
    return LoadBundleFile(bundle->bundle_name);
}

std::size_t ResourcePackageImpl::UnloadAll() noexcept {
    std::vector<std::shared_ptr<ProviderBase>> active;
    std::vector<std::shared_ptr<HandleState>> handle_states;
    {
        std::lock_guard slot_lock(bundle_slot_mutex);
        std::lock_guard lock(mutex);
        ++loading_lock_count;
        for (auto& entry : providers) {
            if (auto provider = entry.second.lock()) {
                active.push_back(std::move(provider));
            }
        }
        providers.clear();
        for (auto& entry : handles) {
            if (auto handle = entry.lock()) {
                handle_states.push_back(std::move(handle));
            }
        }
        handles.clear();
    }
    bundle_slot_condition.notify_all();
    for (auto& handle : handle_states) {
        handle->Release();
    }
    for (auto& provider : active) {
        provider->Invalidate();
    }
    if (scheduler != nullptr && !scheduler->IsUpdating()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            bool pending = false;
            for (const auto& provider : active) {
                if (provider && !provider->IsDone()) {
                    pending = true;
                    break;
                }
            }
            if (!pending || std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            (void)scheduler->Update();
        }
    }
    const std::size_t unloaded = UnloadUnused(std::chrono::milliseconds(0));
    {
        std::lock_guard slot_lock(bundle_slot_mutex);
        std::lock_guard lock(mutex);
        if (loading_lock_count > 0) {
            --loading_lock_count;
        }
    }
    bundle_slot_condition.notify_all();
    return unloaded;
}

std::size_t ResourcePackageImpl::UnloadUnused(std::chrono::milliseconds budget) noexcept {
    constexpr std::size_t kBatch = 4;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    const bool limited = budget > std::chrono::milliseconds(0);
    std::size_t removed = 0;
    for (int round = 0; round < 4; ++round) {
        std::vector<std::shared_ptr<ProviderBase>> invalidated;
        bool progressed = false;
        {
            std::lock_guard lock(mutex);
            std::size_t removed_providers = 0;
            for (auto it = providers.begin(); it != providers.end();) {
                auto provider = it->second.lock();
                if (!provider || (provider->RefCount() == 0 && provider->IsDone())) {
                    if (provider) {
                        invalidated.push_back(std::move(provider));
                    }
                    it = providers.erase(it);
                    ++removed;
                    ++removed_providers;
                    progressed = true;
                    if (limited && removed_providers >= kBatch) {
                        break;
                    }
                } else {
                    ++it;
                }
            }
            if (!limited || invalidated.empty()) {
                std::size_t removed_bundles = 0;
                for (auto it = bundles.begin(); it != bundles.end();) {
                    if (it->second->ReferenceCount() == 0) {
                        it->second->Unload();
                        it = bundles.erase(it);
                        progressed = true;
                        if (limited && ++removed_bundles >= kBatch) {
                            break;
                        }
                    } else {
                        ++it;
                    }
                }
            }
            for (auto it = handles.begin(); it != handles.end();) {
                if (it->expired()) {
                    it = handles.erase(it);
                    progressed = true;
                } else {
                    ++it;
                }
            }
        }
        for (const auto& provider : invalidated) {
            provider->Invalidate();
        }
        if (limited && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        if (!progressed) {
            break;
        }
    }
    return removed;
}

void ResourcePackageImpl::RemoveUnused() noexcept {
    (void)UnloadUnused(std::chrono::milliseconds(0));
}

bool ResourcePackageImpl::TryUnload(std::string_view location) noexcept {
    auto resolved = ResolveAsset(location);
    if (!resolved) {
        return false;
    }
    const std::string prefix = ":" + resolved.Value()->asset_guid.ToHex() + ":";
    std::shared_ptr<ProviderBase> provider;
    {
        std::lock_guard lock(mutex);
        for (auto it = providers.begin(); it != providers.end();) {
            if (it->first.find(prefix) == std::string::npos) {
                ++it;
                continue;
            }
            auto candidate = it->second.lock();
            if (candidate && candidate->RefCount() == 0) {
                provider = std::move(candidate);
                it = providers.erase(it);
                break;
            }
            ++it;
        }
    }
    if (!provider) {
        return false;
    }
    provider->Invalidate();
    return true;
}

RuntimeStats ResourcePackageImpl::Stats() const {
    RuntimeStats stats;
    {
        std::lock_guard lock(mutex);
        for (const auto& entry : providers) {
            if (entry.second.lock()) {
                ++stats.providers;
            }
        }
        for (const auto& weak_handle : handles) {
            auto handle = weak_handle.lock();
            if (handle && !handle->IsReleased()) {
                ++stats.handles;
            }
        }
        stats.bundle_loaders = bundles.size();
        for (const auto& entry : bundles) {
            if (entry.second->IsLoaded()) {
                ++stats.loaded_bundles;
            }
            stats.bundle_bytes += entry.second->Info().file_size;
        }
    }
    if (host) {
        auto cache = host->QueryCache();
        if (cache) {
            stats.cache_bytes = cache.Value().bytes;
        }
    }
    if (scheduler) {
        stats.pending_operations = scheduler->ActiveCount();
    }
    return stats;
}

LeakReport ResourcePackageImpl::Leaks() const {
    LeakReport report;
    std::lock_guard lock(mutex);
    for (const auto& entry : handles) {
        if (auto handle = entry.lock()) {
            if (!handle->IsReleased()) {
                report.handles.push_back(handle->Info().asset_path);
            }
        }
    }
    for (const auto& entry : providers) {
        if (auto provider = entry.second.lock()) {
            if (provider->RefCount() > 0) {
                report.providers.push_back(provider->Key());
            }
        }
    }
    for (const auto& entry : bundles) {
        if (entry.second->ReferenceCount() > 0) {
            report.bundle_loaders.push_back(std::string(entry.second->Info().bundle_name));
        }
    }
    return report;
}

ResourcePackage::ResourcePackage(std::shared_ptr<ResourcePackageImpl> impl)
    : impl_(std::move(impl)) {}

ResourcePackage::~ResourcePackage() { Destroy(); }

Result<std::shared_ptr<ResourcePackage>>
ResourcePackage::Create(const ResourcePackageOptions& options) {
    auto impl = std::make_shared<ResourcePackageImpl>();
    auto package = std::make_shared<ResourcePackage>(impl);
    auto initialized = package->Initialize(options);
    if (!initialized) {
        return std::move(initialized).TakeError();
    }
    return package;
}

Result<void> ResourcePackage::Initialize(const ResourcePackageOptions& options) {
    if (!impl_) {
        return MakeError(ErrorCode::InvalidArgument, "resource package implementation is null");
    }
    return impl_->Initialize(options, nullptr, nullptr);
}

void ResourcePackage::Destroy() noexcept {
    if (impl_) {
        impl_->Destroy();
    }
}

std::string_view ResourcePackage::Name() const noexcept {
    static const std::string empty;
    return impl_ ? std::string_view(impl_->name) : std::string_view(empty);
}

std::string_view ResourcePackage::Version() const noexcept {
    static const std::string empty;
    return impl_ ? std::string_view(impl_->version) : std::string_view(empty);
}

ManifestPtr ResourcePackage::Manifest() const noexcept {
    if (!impl_) {
        return nullptr;
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->manifest;
}

std::shared_ptr<FileSystemHost> ResourcePackage::Host() const noexcept {
    return impl_ ? impl_->host : nullptr;
}

Result<void> ResourcePackage::ReloadManifest() {
    if (!impl_ || !impl_->host) {
        return MakeError(ErrorCode::InvalidArgument, "resource package is not initialized");
    }
    auto loaded = impl_->host->LoadManifest();
    if (!loaded) {
        return std::move(loaded).TakeError();
    }
    std::lock_guard lock(impl_->mutex);
    impl_->manifest = std::move(loaded).Value();
    ++impl_->generation;
    impl_->version.assign(impl_->manifest->PackageVersion());
    if (impl_->name.empty()) {
        impl_->name.assign(impl_->manifest->PackageName());
    }
    return {};
}

Result<void> ResourcePackage::PrefetchManifest() { return ReloadManifest(); }

Result<void> ResourcePackage::LoadManifestFromBytes(std::span<const std::byte> bytes) {
    if (!impl_) {
        return MakeError(ErrorCode::InvalidArgument, "resource package implementation is null");
    }
    std::vector<std::byte> owned(bytes.begin(), bytes.end());
    auto loaded = OpenManifestBytes(std::move(owned));
    if (!loaded) {
        return std::move(loaded).TakeError();
    }
    std::lock_guard lock(impl_->mutex);
    impl_->manifest = std::move(loaded).Value();
    ++impl_->generation;
    impl_->version.assign(impl_->manifest->PackageVersion());
    if (impl_->name.empty()) {
        impl_->name.assign(impl_->manifest->PackageName());
    }
    return {};
}

Result<void> ResourcePackage::LoadManifest(ManifestPtr manifest) {
    if (!impl_) {
        return MakeError(ErrorCode::InvalidArgument, "resource package implementation is null");
    }
    if (!manifest) {
        return MakeError(ErrorCode::InvalidArgument, "remote manifest is null")
            .WithPackage(impl_->name);
    }
    std::lock_guard lock(impl_->mutex);
    impl_->manifest = std::move(manifest);
    ++impl_->generation;
    impl_->version.assign(impl_->manifest->PackageVersion());
    if (impl_->name.empty()) {
        impl_->name.assign(impl_->manifest->PackageName());
    }
    return {};
}

Result<void> ResourcePackage::EnsureBundle(std::string_view bundle_name) {
    if (!impl_ || !impl_->host) {
        return MakeError(ErrorCode::InvalidArgument, "resource package is not initialized");
    }
    const auto* bundle = impl_->FindBundle(bundle_name);
    if (!bundle) {
        return MakeError(ErrorCode::NotFound, "bundle was not found")
            .WithPackage(impl_->name)
            .WithBundle(std::string(bundle_name));
    }
    ManifestPtr manifest;
    {
        std::lock_guard lock(impl_->mutex);
        manifest = impl_->manifest;
    }
    if (!manifest) {
        return MakeError(ErrorCode::InvalidArgument, "package manifest is not loaded")
            .WithPackage(impl_->name);
    }
    const auto* current = manifest->FindBundleByGuid(bundle->bundle_guid);
    if (!current) {
        return MakeError(ErrorCode::NotFound, "bundle was not found")
            .WithPackage(impl_->name)
            .WithBundle(std::string(bundle_name));
    }
    const auto index = static_cast<std::uint32_t>(current - manifest->Bundles().data());
    PARCEL_TRY(impl_->EnsureBundleDownloaded(manifest, index));
    return impl_->host->EnsureBundle(*current);
}

Result<void> ResourcePackage::EnsureBundle(const core::AssetGuid& bundle_guid) {
    if (!impl_ || !impl_->manifest) {
        return MakeError(ErrorCode::InvalidArgument, "resource package is not initialized");
    }
    ManifestPtr manifest;
    {
        std::lock_guard lock(impl_->mutex);
        manifest = impl_->manifest;
    }
    if (!manifest) {
        return MakeError(ErrorCode::InvalidArgument, "package manifest is not loaded");
    }
    const auto* bundle = manifest->FindBundleByGuid(bundle_guid);
    if (!bundle) {
        return MakeError(ErrorCode::NotFound, "bundle GUID was not found")
            .WithPackage(impl_->name)
            .WithBundle(bundle_guid.ToHex());
    }
    const auto index = static_cast<std::uint32_t>(bundle - manifest->Bundles().data());
    PARCEL_TRY(impl_->EnsureBundleDownloaded(manifest, index));
    return impl_->host->EnsureBundle(*bundle);
}

AssetHandle ResourcePackage::LoadAsset(std::string_view location, assets::TypeId asset_type,
                                       int priority) {
    if (!impl_) {
        return AssetHandle{};
    }
    return impl_->Load(location, LoadMethod::Asset, asset_type, false, priority);
}

AssetHandle ResourcePackage::LoadAssetAsync(std::string_view location, assets::TypeId asset_type,
                                            int priority) {
    if (!impl_) {
        return AssetHandle{};
    }
    return impl_->Load(location, LoadMethod::Asset, asset_type, true, priority);
}

AssetHandle ResourcePackage::LoadSubAssets(std::string_view location, assets::TypeId asset_type,
                                           int priority) {
    if (!impl_) {
        return AssetHandle{};
    }
    return impl_->Load(location, LoadMethod::SubAssets, asset_type, false, priority);
}

AssetHandle ResourcePackage::LoadSubAssetsAsync(std::string_view location,
                                                assets::TypeId asset_type, int priority) {
    if (!impl_) {
        return AssetHandle{};
    }
    return impl_->Load(location, LoadMethod::SubAssets, asset_type, true, priority);
}

AssetHandle ResourcePackage::LoadAllAssets(std::string_view location, assets::TypeId asset_type,
                                           int priority) {
    if (!impl_) {
        return AssetHandle{};
    }
    return impl_->Load(location, LoadMethod::AllAssets, asset_type, false, priority);
}

AssetHandle ResourcePackage::LoadAllAssetsAsync(std::string_view location,
                                                assets::TypeId asset_type, int priority) {
    if (!impl_) {
        return AssetHandle{};
    }
    return impl_->Load(location, LoadMethod::AllAssets, asset_type, true, priority);
}

AssetHandle ResourcePackage::LoadRawFile(std::string_view location, int priority) {
    if (!impl_) {
        return AssetHandle{};
    }
    return impl_->Load(location, LoadMethod::RawFile, assets::kInvalidTypeId, false, priority);
}

AssetHandle ResourcePackage::LoadRawFileAsync(std::string_view location, int priority) {
    if (!impl_) {
        return AssetHandle{};
    }
    return impl_->Load(location, LoadMethod::RawFile, assets::kInvalidTypeId, true, priority);
}

AssetHandle ResourcePackage::LoadBundleFile(std::string_view bundle_name) {
    return impl_ ? impl_->LoadBundleFile(bundle_name) : AssetHandle{};
}

AssetHandle ResourcePackage::LoadBundleFile(const core::AssetGuid& bundle_guid) {
    return impl_ ? impl_->LoadBundleFile(bundle_guid) : AssetHandle{};
}

AssetHandle ResourcePackage::LoadBundleFileAsync(std::string_view bundle_name) {
    return LoadBundleFile(bundle_name);
}

AssetHandle ResourcePackage::LoadBundleFileAsync(const core::AssetGuid& bundle_guid) {
    return LoadBundleFile(bundle_guid);
}

AssetHandle ResourcePackage::LoadScene(std::string_view location, int priority) {
    if (!impl_ || !impl_->HasSceneLoader()) {
        return impl_ ? impl_->MakeFailedHandle(
                           AssetInfo{}, LoadMethod::Scene,
                           MakeError(ErrorCode::Unsupported, "no scene loader is configured")
                               .WithPackage(impl_->name))
                     : AssetHandle{};
    }
    return impl_->Load(location, LoadMethod::Scene, assets::kInvalidTypeId, false, priority);
}

AssetHandle ResourcePackage::LoadSceneAsync(std::string_view location, int priority) {
    if (!impl_ || !impl_->HasSceneLoader()) {
        return impl_ ? impl_->MakeFailedHandle(
                           AssetInfo{}, LoadMethod::Scene,
                           MakeError(ErrorCode::Unsupported, "no scene loader is configured")
                               .WithPackage(impl_->name))
                     : AssetHandle{};
    }
    return impl_->Load(location, LoadMethod::Scene, assets::kInvalidTypeId, true, priority);
}

Result<void> ResourcePackage::UnloadScene(const AssetHandle& handle) {
    if (!impl_) {
        return MakeError(ErrorCode::InvalidArgument, "resource package is not initialized");
    }
    if (!handle.IsValid()) {
        return MakeError(ErrorCode::InvalidArgument, "scene handle is invalid");
    }
    if (handle.Method() != LoadMethod::Scene) {
        return MakeError(ErrorCode::InvalidArgument, "asset handle is not a scene handle")
            .WithPackage(impl_->name);
    }
    AssetHandle copy = handle;
    copy.Release();
    return {};
}

void ResourcePackage::SetSceneLoader(std::shared_ptr<scene::ISceneLoader> loader, void* registry) {
    if (impl_) {
        std::lock_guard lock(impl_->mutex);
        impl_->scene_loader = std::move(loader);
        impl_->scene_registry = registry;
    }
}

std::vector<AssetInfo> ResourcePackage::GetAllAssetInfos() const {
    std::vector<AssetInfo> result;
    if (!impl_) {
        return result;
    }
    ManifestPtr manifest;
    {
        std::lock_guard lock(impl_->mutex);
        manifest = impl_->manifest;
    }
    if (!manifest) {
        return result;
    }
    result.reserve(manifest->Assets().size());
    for (const auto& asset : manifest->Assets()) {
        result.push_back(MakeInfo(asset));
    }
    return result;
}

std::vector<AssetInfo> ResourcePackage::GetAssetInfosByTag(std::string_view tag) const {
    std::vector<AssetInfo> result;
    if (!impl_) {
        return result;
    }
    ManifestPtr manifest;
    {
        std::lock_guard lock(impl_->mutex);
        manifest = impl_->manifest;
    }
    if (!manifest) {
        return result;
    }
    for (const auto& asset : manifest->Assets()) {
        if (HasTag(asset, tag)) {
            result.push_back(MakeInfo(asset));
        }
    }
    return result;
}

std::optional<AssetInfo> ResourcePackage::GetAssetInfoByGuid(const core::AssetGuid& guid) const {
    if (!impl_) {
        return std::nullopt;
    }
    ManifestPtr manifest;
    {
        std::lock_guard lock(impl_->mutex);
        manifest = impl_->manifest;
    }
    if (!manifest) {
        return std::nullopt;
    }
    const auto* asset = manifest->FindAssetByGuid(guid);
    if (!asset) {
        return std::nullopt;
    }
    return MakeInfo(*asset);
}

std::optional<AssetInfo> ResourcePackage::GetAssetInfo(std::string_view location) const {
    if (!impl_) {
        return std::nullopt;
    }
    auto resolved = impl_->ResolveAsset(location);
    if (!resolved) {
        return std::nullopt;
    }
    return MakeInfo(*resolved.Value());
}

bool ResourcePackage::IsLocationValid(std::string_view location) const {
    return impl_ && impl_->ResolveAsset(location).HasValue();
}

namespace {

void AddBundleTree(const serialize::ManifestView& manifest, std::uint32_t bundle_index,
                   std::unordered_set<std::uint32_t>& out) {
    if (bundle_index >= manifest.Bundles().size() || !out.insert(bundle_index).second) {
        return;
    }
    for (const std::uint32_t dependency : manifest.Bundles()[bundle_index].dependent_bundle_ids) {
        AddBundleTree(manifest, dependency, out);
    }
}

} // namespace

std::uint64_t ResourcePackage::GetDownloadSize() const {
    if (!impl_) {
        return 0;
    }
    ManifestPtr manifest;
    std::shared_ptr<FileSystemHost> host;
    {
        std::lock_guard lock(impl_->mutex);
        manifest = impl_->manifest;
        host = impl_->host;
    }
    if (!manifest || !host) {
        return 0;
    }
    std::uint64_t total = 0;
    for (const auto& bundle : manifest->Bundles()) {
        if (host->IsDownloadRequired(bundle)) {
            total += bundle.file_size;
        }
    }
    return total;
}

std::uint64_t ResourcePackage::GetDownloadSize(std::string_view location) const {
    if (!impl_) {
        return 0;
    }
    auto resolved = impl_->ResolveAsset(location);
    if (!resolved) {
        return 0;
    }
    ManifestPtr manifest;
    std::shared_ptr<FileSystemHost> host;
    {
        std::lock_guard lock(impl_->mutex);
        manifest = impl_->manifest;
        host = impl_->host;
    }
    if (!manifest || !host) {
        return 0;
    }
    std::unordered_set<std::uint32_t> ids;
    for (const std::uint32_t id : CollectBundleIds(*manifest, *resolved.Value())) {
        AddBundleTree(*manifest, id, ids);
    }
    std::uint64_t total = 0;
    for (const std::uint32_t id : ids) {
        if (host->IsDownloadRequired(manifest->Bundles()[id])) {
            total += manifest->Bundles()[id].file_size;
        }
    }
    return total;
}

std::uint64_t ResourcePackage::GetDownloadSize(std::span<const std::string> locations) const {
    std::uint64_t total = 0;
    for (const auto& location : locations) {
        total += GetDownloadSize(location);
    }
    return total;
}

std::uint64_t ResourcePackage::GetDownloadSizeByTags(std::span<const std::string> tags) const {
    if (!impl_) {
        return 0;
    }
    ManifestPtr manifest;
    std::shared_ptr<FileSystemHost> host;
    {
        std::lock_guard lock(impl_->mutex);
        manifest = impl_->manifest;
        host = impl_->host;
    }
    if (!manifest || !host) {
        return 0;
    }
    std::unordered_set<std::uint32_t> ids;
    for (const auto& asset : manifest->Assets()) {
        bool matches = false;
        for (const auto& tag : asset.tags) {
            if (IsCacheRelevantTag(tag, std::vector<std::string>(tags.begin(), tags.end()))) {
                matches = true;
                break;
            }
        }
        if (!matches) {
            continue;
        }
        for (const std::uint32_t id : CollectBundleIds(*manifest, asset)) {
            AddBundleTree(*manifest, id, ids);
        }
    }
    std::uint64_t total = 0;
    for (const std::uint32_t id : ids) {
        if (host->IsDownloadRequired(manifest->Bundles()[id])) {
            total += manifest->Bundles()[id].file_size;
        }
    }
    return total;
}

std::size_t ResourcePackage::UnloadAllAssets() { return impl_ ? impl_->UnloadAll() : 0; }

std::size_t ResourcePackage::UnloadUnusedAssets(std::chrono::milliseconds budget) {
    return impl_ ? impl_->UnloadUnused(budget) : 0;
}

bool ResourcePackage::TryUnloadUnusedAsset(std::string_view location) {
    return impl_ && impl_->TryUnload(location);
}

Result<void> ResourcePackage::ClearCache(const CacheClearOptions& options) {
    if (!impl_ || !impl_->host) {
        return MakeError(ErrorCode::InvalidArgument, "resource package is not initialized");
    }
    CacheClearOptions resolved = options;
    if (options.mode == CacheClearMode::OldVersions) {
        ManifestPtr manifest;
        {
            std::lock_guard lock(impl_->mutex);
            manifest = impl_->manifest;
            for (const auto& [guid, bundle] : impl_->bundles) {
                (void)guid;
                if (bundle) {
                    resolved.keep_file_names.emplace_back(std::string(bundle->Info().file_name));
                }
            }
        }
        if (!manifest) {
            return MakeError(ErrorCode::InvalidArgument, "package manifest is not loaded")
                .WithPackage(impl_->name);
        }
        for (const auto& bundle : manifest->Bundles()) {
            resolved.keep_file_names.emplace_back(std::string(bundle.file_name));
        }
        return impl_->host->ClearCache(resolved);
    }
    if ((options.mode == CacheClearMode::ByTags || options.mode == CacheClearMode::ByLocations ||
         options.mode == CacheClearMode::Unused) &&
        resolved.file_names.empty()) {
        ManifestPtr manifest;
        {
            std::lock_guard lock(impl_->mutex);
            manifest = impl_->manifest;
        }
        if (!manifest) {
            return MakeError(ErrorCode::InvalidArgument, "package manifest is not loaded");
        }
        std::unordered_set<std::uint32_t> ids;
        if (options.mode == CacheClearMode::ByLocations) {
            for (const auto& location : options.locations) {
                auto asset = impl_->ResolveAsset(location);
                if (!asset) {
                    continue;
                }
                for (const std::uint32_t id : CollectBundleIds(*manifest, *asset.Value())) {
                    AddBundleTree(*manifest, id, ids);
                }
            }
        } else if (options.mode == CacheClearMode::Unused) {
            std::lock_guard lock(impl_->mutex);
            for (std::uint32_t i = 0; i < manifest->Bundles().size(); ++i) {
                const auto found = impl_->bundles.find(manifest->Bundles()[i].bundle_guid);
                if (found == impl_->bundles.end() || found->second->ReferenceCount() == 0) {
                    ids.insert(i);
                }
            }
        } else {
            for (const auto& asset : manifest->Assets()) {
                for (const auto& tag : asset.tags) {
                    if (std::find(options.tags.begin(), options.tags.end(), tag) !=
                        options.tags.end()) {
                        for (const std::uint32_t id : CollectBundleIds(*manifest, asset)) {
                            AddBundleTree(*manifest, id, ids);
                        }
                        break;
                    }
                }
            }
        }
        for (const std::uint32_t id : ids) {
            resolved.file_names.emplace_back(manifest->Bundles()[id].file_name);
        }
        if (resolved.file_names.empty() && options.mode != CacheClearMode::All &&
            options.mode != CacheClearMode::OldVersions) {
            return {};
        }
    }
    return impl_->host->ClearCache(resolved);
}

Result<CacheVerifyReport> ResourcePackage::VerifyCacheFiles(const CacheVerifyOptions& options) {
    if (!impl_ || !impl_->host) {
        return MakeError(ErrorCode::InvalidArgument, "resource package is not initialized");
    }
    ManifestPtr manifest;
    {
        std::lock_guard lock(impl_->mutex);
        manifest = impl_->manifest;
    }
    if (!manifest) {
        return MakeError(ErrorCode::InvalidArgument, "package manifest is not loaded")
            .WithPackage(impl_->name);
    }

    const auto host = impl_->host;
    const auto bundles = manifest->Bundles();
    CacheVerifyReport report;
    report.total = bundles.size();
    std::vector<CacheVerifyStatus> statuses(report.total, CacheVerifyStatus::Unsupported);
    std::vector<std::uint8_t> failures(report.total, 0U);

    const std::size_t configured_workers =
        std::clamp<std::size_t>(options.max_concurrency, std::size_t{1}, std::size_t{32});
    const std::size_t hardware = std::thread::hardware_concurrency();
    const std::size_t hardware_cap = hardware == 0 ? 1U : hardware * 2U + 1U;
    const std::size_t worker_count =
        std::min({configured_workers, hardware_cap, report.total});
    auto verify_one = [&](std::size_t index) {
        auto status = host->VerifyCachedBundle(bundles[index], options.level);
        if (!status) {
            failures[index] = 1U;
            return;
        }
        statuses[index] = status.Value();
    };

    if (worker_count > 1) {
        std::atomic<std::size_t> next{0};
        auto worker = [&] {
            for (;;) {
                const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= report.total) {
                    return;
                }
                verify_one(index);
            }
        };
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers.emplace_back(worker);
        }
        for (auto& thread : workers) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    } else if (worker_count == 1) {
        for (std::size_t i = 0; i < report.total; ++i) {
            verify_one(i);
        }
    }

    for (std::size_t i = 0; i < report.total; ++i) {
        if (failures[i] != 0U) {
            ++report.failed;
            continue;
        }
        const auto& bundle = bundles[i];
        switch (statuses[i]) {
            case CacheVerifyStatus::Valid:
                ++report.valid;
                report.valid_bytes += bundle.file_size;
                break;
            case CacheVerifyStatus::Missing:
                ++report.missing;
                break;
            case CacheVerifyStatus::Corrupt:
                ++report.invalid;
                if (host->InvalidateBundle(bundle)) {
                    ++report.removed;
                } else {
                    ++report.failed;
                }
                break;
            case CacheVerifyStatus::Unsupported:
                ++report.unsupported;
                break;
        }
    }
    return report;
}

Result<void> ResourcePackage::VerifyCache() {
    auto result = VerifyCacheFiles(CacheVerifyOptions{});
    if (!result) {
        return std::move(result).TakeError();
    }
    const CacheVerifyReport& report = result.Value();
    if (report.failed > 0) {
        return MakeError(ErrorCode::Corrupt, "cache verification failed for some bundles")
            .WithPackage(impl_->name);
    }
    return {};
}


Result<CacheStats> ResourcePackage::QueryCache() const {
    if (!impl_ || !impl_->host) {
        return MakeError(ErrorCode::InvalidArgument, "resource package is not initialized");
    }
    return impl_->host->QueryCache();
}

Result<std::shared_ptr<ResourceDownloader>>
ResourcePackage::CreateDownloader(std::span<const std::string> locations,
                                  std::span<const std::string> tags) {
    if (!impl_ || !impl_->host) {
        return MakeError(ErrorCode::InvalidArgument, "resource package is not initialized");
    }
    if (!impl_->options.download_transport || !impl_->options.download_url_provider) {
        return MakeError(ErrorCode::Unsupported,
                         "download transport and URL provider are not configured")
            .WithPackage(impl_->name);
    }

    ManifestPtr manifest;
    {
        std::lock_guard lock(impl_->mutex);
        manifest = impl_->manifest;
    }
    if (!manifest) {
        return MakeError(ErrorCode::InvalidArgument, "package manifest is not loaded")
            .WithPackage(impl_->name);
    }

    std::shared_ptr<IFileSystem> download_file_system = impl_->options.download_file_system;
    if (!download_file_system) {
        for (const auto& file_system : impl_->host->FileSystems()) {
            if (file_system && file_system->SupportsBundleDownload()) {
                download_file_system = file_system;
                break;
            }
        }
    }
    if (!download_file_system) {
        return MakeError(ErrorCode::Unsupported, "no file system supports downloading bundles")
            .WithPackage(impl_->name);
    }

    std::unordered_set<std::uint32_t> ids;
    if (locations.empty() && tags.empty()) {
        for (std::uint32_t i = 0; i < manifest->Bundles().size(); ++i) {
            if (impl_->host->IsDownloadRequired(manifest->Bundles()[i])) {
                ids.insert(i);
            }
        }
    } else {
        for (const auto& location : locations) {
            auto asset = impl_->ResolveAsset(location);
            if (!asset) {
                return MakeError(ErrorCode::NotFound, "download location was not found")
                    .WithPackage(impl_->name)
                    .WithAsset(std::string(location));
            }
            for (const std::uint32_t id : CollectBundleIds(*manifest, *asset.Value())) {
                AddBundleTree(*manifest, id, ids);
            }
        }
        for (const auto& asset : manifest->Assets()) {
            bool matches = false;
            for (const auto& tag : asset.tags) {
                if (std::find(tags.begin(), tags.end(), tag) != tags.end()) {
                    matches = true;
                    break;
                }
            }
            if (!matches) {
                continue;
            }
            for (const std::uint32_t id : CollectBundleIds(*manifest, asset)) {
                AddBundleTree(*manifest, id, ids);
            }
        }
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(ids.size());
    for (const std::uint32_t id : ids) {
        if (id < manifest->Bundles().size() &&
            impl_->host->IsDownloadRequired(manifest->Bundles()[id])) {
            indices.push_back(id);
        }
    }
    std::sort(indices.begin(), indices.end());

    return MakeBundleDownloader(
        impl_->name, std::string(manifest->PackageVersion()), manifest, indices, impl_->host,
        std::move(download_file_system), impl_->options.download_transport,
        impl_->options.download_url_provider, impl_->options.download_options);
}

Result<std::shared_ptr<ResourceUnpacker>> ResourcePackage::CreateUnpacker() {
    return MakeError(ErrorCode::Unsupported, "unpacker is not available in R05")
        .WithPackage(impl_ ? impl_->name : std::string{});
}

Result<std::shared_ptr<ResourceImporter>> ResourcePackage::CreateImporter() {
    return MakeError(ErrorCode::Unsupported, "importer is not available in R05")
        .WithPackage(impl_ ? impl_->name : std::string{});
}

Result<void> ResourcePackage::StartOperation(core::OperationPtr operation) {
    if (!impl_ || impl_->scheduler == nullptr) {
        return MakeError(ErrorCode::InvalidArgument, "resource package has no scheduler");
    }
    if (!operation) {
        return MakeError(ErrorCode::InvalidArgument, "operation is null");
    }
    impl_->scheduler->Add(std::move(operation));
    return {};
}

RuntimeStats ResourcePackage::Stats() const { return impl_ ? impl_->Stats() : RuntimeStats{}; }

LeakReport ResourcePackage::Leaks() const { return impl_ ? impl_->Leaks() : LeakReport{}; }

std::string ResourcePackage::LeakReportText() const { return Leaks().ToString(); }

void ResourcePackage::SetPriority(int priority) noexcept {
    if (impl_) {
        std::lock_guard lock(impl_->mutex);
        impl_->priority = priority;
    }
}

int ResourcePackage::Priority() const noexcept {
    if (!impl_) {
        return 0;
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->priority;
}

void ResourcePackage::SetAutoUnloadBundleWhenUnused(bool value) noexcept {
    if (impl_) {
        std::lock_guard lock(impl_->mutex);
        impl_->auto_unload = value;
    }
}

bool ResourcePackage::AutoUnloadBundleWhenUnused() const noexcept {
    return impl_ && impl_->auto_unload;
}

bool ResourcePackage::IsLoadingLocked() const noexcept { return impl_ && impl_->loaded_lock(); }

void ResourcePackage::AcquireLoadingLock() noexcept {
    if (impl_) {
        {
            std::lock_guard slot_lock(impl_->bundle_slot_mutex);
            std::lock_guard lock(impl_->mutex);
            ++impl_->loading_lock_count;
        }
        impl_->bundle_slot_condition.notify_all();
    }
}

void ResourcePackage::ReleaseLoadingLock() noexcept {
    if (impl_) {
        {
            std::lock_guard slot_lock(impl_->bundle_slot_mutex);
            std::lock_guard lock(impl_->mutex);
            if (impl_->loading_lock_count > 0) {
                --impl_->loading_lock_count;
            }
        }
        impl_->bundle_slot_condition.notify_all();
    }
}

LoadingLock::LoadingLock(ResourcePackage& package) noexcept : package_(&package) {
    package_->AcquireLoadingLock();
}

LoadingLock::~LoadingLock() { Release(); }

LoadingLock::LoadingLock(LoadingLock&& other) noexcept : package_(other.package_) {
    other.package_ = nullptr;
}

LoadingLock& LoadingLock::operator=(LoadingLock&& other) noexcept {
    if (this != &other) {
        Release();
        package_ = other.package_;
        other.package_ = nullptr;
    }
    return *this;
}

void LoadingLock::Release() noexcept {
    if (package_) {
        package_->ReleaseLoadingLock();
        package_ = nullptr;
    }
}

} // namespace parcel::runtime




