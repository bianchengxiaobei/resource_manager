#include "runtime_internal.hpp"

#include "parcel/core/platform_file.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace parcel::runtime {
namespace {

using core::ErrorCode;
using core::MakeError;
using core::OperationStatus;
using core::Result;

struct TransferTask {
    serialize::ManifestBundleView bundle;
    std::string label;
    std::string source_path;
    std::uint64_t bytes = 0;
};

class TransferState {
  public:
    TransferState(ManifestPtr manifest, std::vector<TransferTask> tasks, int max_concurrency,
                  std::function<Result<void>(const TransferTask&)> runner)
        : manifest_(std::move(manifest)), tasks_(std::move(tasks)), runner_(std::move(runner)),
          max_concurrency_(std::clamp(max_concurrency, 1, 16)) {
        for (const TransferTask& task : tasks_) {
            total_bytes_ += task.bytes;
        }
    }

    ~TransferState() {
        Cancel();
        JoinWorkers();
    }

    TransferState(const TransferState&) = delete;
    TransferState& operator=(const TransferState&) = delete;

    void Start() {
        if (started_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (canceled_.load(std::memory_order_acquire)) {
            done_.store(true, std::memory_order_release);
            return;
        }
        if (tasks_.empty()) {
            done_.store(true, std::memory_order_release);
            return;
        }
        const std::size_t count =
            std::min(tasks_.size(), static_cast<std::size_t>(max_concurrency_));
        active_workers_.store(static_cast<int>(count), std::memory_order_release);
        workers_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    void Cancel() {
        canceled_.store(true, std::memory_order_release);
        if (!started_.load(std::memory_order_acquire)) {
            done_.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] bool IsDone() const noexcept { return done_.load(std::memory_order_acquire); }

    [[nodiscard]] OperationStatus Status() const noexcept {
        if (canceled_.load(std::memory_order_acquire)) {
            return OperationStatus::Canceled;
        }
        if (!IsDone()) {
            return OperationStatus::Running;
        }
        if (aborted_.load(std::memory_order_acquire)) {
            return OperationStatus::Failed;
        }
        return OperationStatus::Succeeded;
    }

    [[nodiscard]] float Progress() const noexcept {
        if (total_bytes_ != 0) {
            return std::clamp(static_cast<float>(completed_bytes_.load(std::memory_order_acquire)) /
                                  static_cast<float>(total_bytes_),
                              0.0f, 1.0f);
        }
        if (tasks_.empty()) {
            return 1.0f;
        }
        return static_cast<float>(completed_.load(std::memory_order_acquire)) /
               static_cast<float>(tasks_.size());
    }

    [[nodiscard]] const core::Error& LastError() const noexcept {
        std::lock_guard lock(error_mutex_);
        if (last_error_.IsOk()) {
            return empty_error_;
        }
        error_copy_ = last_error_;
        return error_copy_;
    }

  private:
    void SetError(core::Error error) {
        std::lock_guard lock(error_mutex_);
        if (last_error_.IsOk()) {
            last_error_ = std::move(error);
        }
        aborted_.store(true, std::memory_order_release);
    }

    void JoinWorkers() {
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

    void WorkerLoop() {
        for (;;) {
            if (canceled_.load(std::memory_order_acquire) ||
                aborted_.load(std::memory_order_acquire)) {
                break;
            }
            const std::size_t index = next_.fetch_add(1, std::memory_order_acq_rel);
            if (index >= tasks_.size()) {
                break;
            }
            const TransferTask& task = tasks_[index];
            auto result = runner_(task);
            if (!result) {
                SetError(result.GetError());
                break;
            }
            completed_bytes_.fetch_add(task.bytes, std::memory_order_acq_rel);
            completed_.fetch_add(1, std::memory_order_acq_rel);
        }
        if (active_workers_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            done_.store(true, std::memory_order_release);
        }
    }

    ManifestPtr manifest_;
    std::vector<TransferTask> tasks_;
    std::function<Result<void>(const TransferTask&)> runner_;
    std::vector<std::thread> workers_;
    const int max_concurrency_;
    std::atomic<std::size_t> next_{0};
    std::atomic<std::size_t> completed_{0};
    std::atomic<std::uint64_t> completed_bytes_{0};
    std::uint64_t total_bytes_ = 0;
    std::atomic<bool> started_{false};
    std::atomic<bool> canceled_{false};
    std::atomic<bool> aborted_{false};
    std::atomic<bool> done_{false};
    std::atomic<int> active_workers_{0};
    mutable std::mutex error_mutex_;
    core::Error last_error_{ErrorCode::Ok};
    mutable core::Error empty_error_{ErrorCode::Ok};
    mutable core::Error error_copy_{ErrorCode::Ok};
};

class BundleUnpacker final : public ResourceUnpacker {
  public:
    BundleUnpacker(ManifestPtr manifest, std::vector<TransferTask> tasks, int max_concurrency,
                   std::function<Result<void>(const TransferTask&)> runner)
        : state_(std::move(manifest), std::move(tasks), max_concurrency, std::move(runner)) {}

    void Start() override { state_.Start(); }
    void Cancel() override { state_.Cancel(); }
    [[nodiscard]] bool IsDone() const override { return state_.IsDone(); }
    [[nodiscard]] OperationStatus Status() const override { return state_.Status(); }
    [[nodiscard]] float Progress() const override { return state_.Progress(); }
    [[nodiscard]] const core::Error& LastError() const override { return state_.LastError(); }

  private:
    TransferState state_;
};

class BundleImporter final : public ResourceImporter {
  public:
    BundleImporter(ManifestPtr manifest, std::vector<TransferTask> tasks, int max_concurrency,
                   std::function<Result<void>(const TransferTask&)> runner)
        : state_(std::move(manifest), std::move(tasks), max_concurrency, std::move(runner)) {}

    void Start() override { state_.Start(); }
    void Cancel() override { state_.Cancel(); }
    [[nodiscard]] bool IsDone() const override { return state_.IsDone(); }
    [[nodiscard]] OperationStatus Status() const override { return state_.Status(); }
    [[nodiscard]] float Progress() const override { return state_.Progress(); }
    [[nodiscard]] const core::Error& LastError() const override { return state_.LastError(); }

  private:
    TransferState state_;
};

bool MatchesTags(const serialize::ManifestBundleView& bundle,
                 const std::vector<std::string>& tags) {
    if (tags.empty()) {
        return true;
    }
    for (const std::string_view tag : bundle.tags) {
        if (std::find(tags.begin(), tags.end(), tag) != tags.end()) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<IFileSystem> FindBuiltinFileSystem(const std::shared_ptr<FileSystemHost>& host) {
    for (const auto& file_system : host->FileSystems()) {
        if (file_system && file_system->Name() == "builtin") {
            return file_system;
        }
    }
    return nullptr;
}

std::shared_ptr<IFileSystem> FindImportFileSystem(const std::shared_ptr<FileSystemHost>& host) {
    for (const auto& file_system : host->FileSystems()) {
        if (file_system && file_system->SupportsBundleImport()) {
            return file_system;
        }
    }
    return nullptr;
}

Result<std::vector<std::byte>> ReadBundleFile(std::string_view path) {
    core::PlatformFile platform;
    auto opened = platform.OpenRead(path);
    if (!opened) {
        return std::move(opened).TakeError().WithPath(std::string(path));
    }
    auto bytes = core::ReadAll(*opened.Value());
    opened.Value()->Close();
    if (!bytes) {
        return std::move(bytes).TakeError().WithPath(std::string(path));
    }
    return bytes;
}

const serialize::ManifestBundleView*
ResolveImportBundle(const ManifestPtr& manifest, const BundleImportInfo& info, core::Error& error) {
    const serialize::ManifestBundleView* bundle = nullptr;
    if (!info.bundle_guid.IsZero()) {
        bundle = manifest->FindBundleByGuid(info.bundle_guid);
    } else if (!info.bundle_name.empty()) {
        bundle = manifest->FindBundleByName(info.bundle_name);
    } else {
        error =
            MakeError(ErrorCode::InvalidArgument, "bundle import requires a bundle name or GUID");
        return nullptr;
    }
    if (bundle == nullptr) {
        error =
            MakeError(ErrorCode::NotFound, "bundle import target is not present in the manifest")
                .WithBundle(info.bundle_name.empty() ? info.bundle_guid.ToHex() : info.bundle_name);
        return nullptr;
    }
    if (!info.bundle_name.empty() && bundle->bundle_name != info.bundle_name) {
        error = MakeError(ErrorCode::InvalidArgument,
                          "bundle import name does not match the bundle GUID")
                    .WithBundle(info.bundle_name);
        return nullptr;
    }
    if (info.file_path.empty()) {
        error = MakeError(ErrorCode::InvalidArgument, "bundle import file path is empty")
                    .WithBundle(std::string(bundle->bundle_name));
        return nullptr;
    }
    return bundle;
}

} // namespace

Result<std::shared_ptr<ResourceUnpacker>> MakeBundleUnpacker(std::string package_name,
                                                             ManifestPtr manifest,
                                                             std::shared_ptr<FileSystemHost> host,
                                                             UnpackOptions options) {
    if (!manifest || !host) {
        return MakeError(ErrorCode::InvalidArgument, "resource package is not initialized")
            .WithPackage(std::move(package_name));
    }
    auto source = FindBuiltinFileSystem(host);
    if (!source) {
        return MakeError(ErrorCode::Unsupported, "builtin file system is not configured")
            .WithPackage(package_name);
    }
    auto destination = FindImportFileSystem(host);
    if (!destination || destination == source) {
        return MakeError(ErrorCode::Unsupported, "writable bundle file system is not configured")
            .WithPackage(package_name);
    }

    std::vector<TransferTask> tasks;
    tasks.reserve(manifest->Bundles().size());
    for (const serialize::ManifestBundleView& bundle : manifest->Bundles()) {
        if (!MatchesTags(bundle, options.tags) || !source->CanAccept(bundle) ||
            !destination->IsDownloadRequired(bundle)) {
            continue;
        }
        TransferTask task;
        task.bundle = bundle;
        task.label.assign(bundle.bundle_name);
        task.bytes = bundle.file_size;
        tasks.push_back(std::move(task));
    }

    auto runner = [source = std::move(source), destination = std::move(destination),
                   package_name](const TransferTask& task) -> Result<void> {
        auto bytes = source->LoadBundle(task.bundle);
        if (!bytes) {
            return std::move(bytes).TakeError().WithPackage(package_name).WithBundle(task.label);
        }
        auto imported = destination->ImportBundle(
            task.bundle, std::span<const std::byte>(bytes.Value().data(), bytes.Value().size()));
        if (!imported) {
            return std::move(imported).TakeError().WithPackage(package_name).WithBundle(task.label);
        }
        return {};
    };

    std::shared_ptr<ResourceUnpacker> operation = std::make_shared<BundleUnpacker>(
        std::move(manifest), std::move(tasks), options.max_concurrency, std::move(runner));
    return operation;
}

Result<std::shared_ptr<ResourceImporter>> MakeBundleImporter(std::string package_name,
                                                             ManifestPtr manifest,
                                                             std::shared_ptr<FileSystemHost> host,
                                                             BundleImportOptions options) {
    if (!manifest || !host) {
        return MakeError(ErrorCode::InvalidArgument, "resource package is not initialized")
            .WithPackage(std::move(package_name));
    }
    if (options.bundles.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "bundle importer requires at least one bundle")
            .WithPackage(package_name);
    }
    auto destination = FindImportFileSystem(host);
    if (!destination) {
        return MakeError(ErrorCode::Unsupported, "writable bundle file system is not configured")
            .WithPackage(package_name);
    }

    std::vector<TransferTask> tasks;
    std::unordered_set<core::AssetGuid, core::Guid128Hash> seen;
    tasks.reserve(options.bundles.size());
    for (const BundleImportInfo& info : options.bundles) {
        core::Error resolve_error{ErrorCode::Ok};
        const serialize::ManifestBundleView* bundle =
            ResolveImportBundle(manifest, info, resolve_error);
        if (bundle == nullptr) {
            resolve_error.WithPackage(package_name);
            return resolve_error;
        }
        if (!destination->IsDownloadRequired(*bundle) || !seen.insert(bundle->bundle_guid).second) {
            continue;
        }
        TransferTask task;
        task.bundle = *bundle;
        task.label.assign(bundle->bundle_name);
        task.source_path = info.file_path;
        task.bytes = bundle->file_size;
        tasks.push_back(std::move(task));
    }

    auto runner = [destination = std::move(destination),
                   package_name](const TransferTask& task) -> Result<void> {
        auto bytes = ReadBundleFile(task.source_path);
        if (!bytes) {
            return std::move(bytes).TakeError().WithPackage(package_name).WithBundle(task.label);
        }
        auto imported = destination->ImportBundle(
            task.bundle, std::span<const std::byte>(bytes.Value().data(), bytes.Value().size()));
        if (!imported) {
            return std::move(imported).TakeError().WithPackage(package_name).WithBundle(task.label);
        }
        return {};
    };

    std::shared_ptr<ResourceImporter> operation = std::make_shared<BundleImporter>(
        std::move(manifest), std::move(tasks), options.max_concurrency, std::move(runner));
    return operation;
}

} // namespace parcel::runtime
