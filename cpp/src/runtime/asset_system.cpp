#include "parcel/runtime/asset_system.hpp"

#include "runtime_internal.hpp"

#include <mutex>
#include <utility>

namespace parcel::runtime {
namespace {

using core::ErrorCode;
using core::MakeError;
using core::Result;

} // namespace

struct AssetSystemImpl {
    AssetSystemImpl() : scheduler(nullptr, std::chrono::milliseconds(4)) {}

    AssetSystemOptions options;
    std::shared_ptr<core::ThreadPool> pool;
    core::OperationScheduler scheduler;
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<ResourcePackage>> packages;
    bool initialized = false;
};

AssetSystem::AssetSystem() : impl_(std::make_shared<AssetSystemImpl>()) {}

AssetSystem::~AssetSystem() { Shutdown(); }

Result<void> AssetSystem::Initialize(const AssetSystemOptions& options) {
    if (!impl_) {
        return MakeError(ErrorCode::Internal, "asset system implementation is null");
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->initialized) {
        return MakeError(ErrorCode::InvalidArgument, "asset system is already initialized");
    }
    impl_->options = options;
    core::ThreadPoolOptions pool_options;
    pool_options.thread_count = options.worker_threads;
    pool_options.thread_name_prefix = "parcel-runtime";
    impl_->pool = std::make_shared<core::ThreadPool>(pool_options);
    impl_->scheduler.SetFrameBudget(options.frame_budget);
    impl_->initialized = true;
    return {};
}

void AssetSystem::Shutdown() noexcept {
    if (!impl_) {
        return;
    }
    std::vector<std::shared_ptr<ResourcePackage>> packages;
    std::shared_ptr<core::ThreadPool> pool;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->initialized) {
            return;
        }
        impl_->initialized = false;
        for (auto& entry : impl_->packages) {
            packages.push_back(std::move(entry.second));
        }
        impl_->packages.clear();
        pool = std::move(impl_->pool);
    }
    for (auto& package : packages) {
        if (package) {
            package->Destroy();
        }
    }
    impl_->scheduler.CancelAll();
    if (pool) {
        pool->Stop(true);
    }
}

bool AssetSystem::IsInitialized() const noexcept {
    if (!impl_) {
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->initialized;
}

void AssetSystem::Update() {
    if (impl_ && impl_->initialized) {
        (void)impl_->scheduler.Update();
    }
}

void AssetSystem::Update(std::chrono::nanoseconds budget) {
    if (impl_ && impl_->initialized) {
        (void)impl_->scheduler.Update(budget);
    }
}

void AssetSystem::SetFrameBudget(std::chrono::nanoseconds budget) noexcept {
    if (impl_) {
        impl_->scheduler.SetFrameBudget(budget);
    }
}

std::chrono::nanoseconds AssetSystem::FrameBudget() const noexcept {
    return impl_ ? impl_->scheduler.FrameBudget() : std::chrono::nanoseconds::zero();
}

Result<ResourcePackage*> AssetSystem::CreatePackage(const ResourcePackageOptions& options) {
    if (!impl_) {
        return MakeError(ErrorCode::Internal, "asset system implementation is null");
    }
    if (options.package_name.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "package name is empty");
    }
    return CreatePackage(options.package_name, options);
}

Result<ResourcePackage*> AssetSystem::CreatePackage(std::string_view name,
                                                    const ResourcePackageOptions& options) {
    if (!impl_) {
        return MakeError(ErrorCode::Internal, "asset system implementation is null");
    }
    if (name.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "package name is empty");
    }
    std::shared_ptr<core::ThreadPool> pool;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->initialized) {
            return MakeError(ErrorCode::InvalidArgument, "asset system is not initialized");
        }
        if (impl_->packages.contains(std::string(name))) {
            return MakeError(ErrorCode::InvalidArgument, "package already exists")
                .WithPackage(std::string(name));
        }
        pool = impl_->pool;
    }

    ResourcePackageOptions package_options = options;
    package_options.package_name = std::string(name);
    if (package_options.type_registry == nullptr &&
        impl_->options.max_concurrent_bundle_loads > 0 &&
        package_options.max_concurrent_bundle_loads <= 0) {
        package_options.max_concurrent_bundle_loads = impl_->options.max_concurrent_bundle_loads;
    }

    auto package_impl = std::make_shared<ResourcePackageImpl>();
    auto package = std::make_shared<ResourcePackage>(package_impl);
    auto initialized =
        package_impl->Initialize(package_options, std::move(pool), &impl_->scheduler);
    if (!initialized) {
        return std::move(initialized).TakeError();
    }

    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->initialized) {
            package_impl->Destroy();
            return MakeError(ErrorCode::InvalidArgument, "asset system was shut down");
        }
        impl_->packages.emplace(std::string(name), package);
    }
    return package.get();
}

ResourcePackage* AssetSystem::GetPackage(std::string_view name) const noexcept {
    if (!impl_) {
        return nullptr;
    }
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->packages.find(std::string(name));
    return found == impl_->packages.end() ? nullptr : found->second.get();
}

std::shared_ptr<ResourcePackage> AssetSystem::FindPackage(std::string_view name) const noexcept {
    if (!impl_) {
        return nullptr;
    }
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->packages.find(std::string(name));
    return found == impl_->packages.end() ? nullptr : found->second;
}

std::vector<std::string> AssetSystem::PackageNames() const {
    std::vector<std::string> result;
    if (!impl_) {
        return result;
    }
    std::lock_guard lock(impl_->mutex);
    result.reserve(impl_->packages.size());
    for (const auto& entry : impl_->packages) {
        result.push_back(entry.first);
    }
    return result;
}

Result<void> AssetSystem::DestroyPackage(std::string_view name) {
    if (!impl_) {
        return MakeError(ErrorCode::Internal, "asset system implementation is null");
    }
    std::shared_ptr<ResourcePackage> package;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->packages.find(std::string(name));
        if (found == impl_->packages.end()) {
            return {};
        }
        package = std::move(found->second);
        impl_->packages.erase(found);
    }
    if (package) {
        package->Destroy();
    }
    return {};
}

core::OperationScheduler& AssetSystem::Scheduler() noexcept { return impl_->scheduler; }

const core::OperationScheduler& AssetSystem::Scheduler() const noexcept { return impl_->scheduler; }

Result<void> AssetSystem::StartOperation(core::OperationPtr operation) {
    if (!operation) {
        return MakeError(ErrorCode::InvalidArgument, "operation is null");
    }
    if (!impl_) {
        return MakeError(ErrorCode::Internal, "asset system implementation is null");
    }
    impl_->scheduler.Add(std::move(operation));
    return {};
}

RuntimeStats AssetSystem::Stats() const {
    RuntimeStats total;
    if (!impl_) {
        return total;
    }
    std::vector<std::shared_ptr<ResourcePackage>> packages;
    {
        std::lock_guard lock(impl_->mutex);
        total.packages = impl_->packages.size();
        packages.reserve(impl_->packages.size());
        for (const auto& entry : impl_->packages) {
            packages.push_back(entry.second);
        }
    }
    for (const auto& package : packages) {
        if (!package) {
            continue;
        }
        const RuntimeStats stats = package->Stats();
        total.providers += stats.providers;
        total.handles += stats.handles;
        total.bundle_loaders += stats.bundle_loaders;
        total.loaded_bundles += stats.loaded_bundles;
        total.bundle_bytes += stats.bundle_bytes;
        total.cache_bytes += stats.cache_bytes;
        total.download_bytes += stats.download_bytes;
        total.downloaded_bytes += stats.downloaded_bytes;
    }
    total.pending_operations = impl_->scheduler.ActiveCount();
    return total;
}

LeakReport AssetSystem::Leaks() const {
    LeakReport total;
    if (!impl_) {
        return total;
    }
    std::vector<std::shared_ptr<ResourcePackage>> packages;
    {
        std::lock_guard lock(impl_->mutex);
        for (const auto& entry : impl_->packages) {
            packages.push_back(entry.second);
        }
    }
    for (const auto& package : packages) {
        if (!package) {
            continue;
        }
        LeakReport report = package->Leaks();
        total.handles.insert(total.handles.end(), report.handles.begin(), report.handles.end());
        total.providers.insert(total.providers.end(), report.providers.begin(),
                               report.providers.end());
        total.bundle_loaders.insert(total.bundle_loaders.end(), report.bundle_loaders.begin(),
                                    report.bundle_loaders.end());
    }
    return total;
}

std::string AssetSystem::LeakReportText() const { return Leaks().ToString(); }

} // namespace parcel::runtime
