#include "parcel/runtime/download.hpp"

#include "parcel/core/hash.hpp"
#include "parcel/core/path.hpp"
#include "parcel/core/platform_file.hpp"
#include "parcel/runtime/api.hpp"
#include "runtime_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace parcel::runtime {
namespace {

using core::ErrorCode;
using core::MakeError;
using core::Result;

char LowerAsciiChar(char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool EqualsAsciiIgnoreCase(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (LowerAsciiChar(lhs[i]) != LowerAsciiChar(rhs[i])) {
            return false;
        }
    }
    return true;
}

struct SharedDownloadGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool claimed = false;
    bool done = false;
    core::Error error{ErrorCode::Ok};
    std::uint64_t downloaded = 0;
    std::uint64_t total = 0;

    void Complete(core::Error result, std::uint64_t bytes, std::uint64_t size) {
        {
            std::lock_guard lock(mutex);
            error = std::move(result);
            downloaded = bytes;
            total = size;
            done = true;
        }
        condition.notify_all();
    }
};

struct SharedGateRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, std::weak_ptr<SharedDownloadGate>> entries;
};

SharedGateRegistry& GateRegistry() {
    static SharedGateRegistry registry;
    return registry;
}

std::shared_ptr<SharedDownloadGate> SharedDownloadGateFor(std::string_view path) {
    auto& registry = GateRegistry();
    std::lock_guard lock(registry.mutex);
    auto& slot = registry.entries[std::string(path)];
    auto gate = slot.lock();
    if (!gate) {
        gate = std::make_shared<SharedDownloadGate>();
        slot = gate;
    }
    return gate;
}

void ReleaseSharedDownloadGate(std::string_view path,
                              const std::shared_ptr<SharedDownloadGate>& gate) {
    auto& registry = GateRegistry();
    std::lock_guard lock(registry.mutex);
    auto found = registry.entries.find(std::string(path));
    if (found == registry.entries.end()) {
        return;
    }
    auto current = found->second.lock();
    if (!current || current == gate) {
        registry.entries.erase(found);
    }
}

std::optional<std::uint64_t> ParseUnsigned(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        result = result * 10 + digit;
    }
    return result;
}

std::uint64_t NowNanoseconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

class ProgressWatchdog {
  public:
    explicit ProgressWatchdog(std::chrono::milliseconds timeout)
        : timeout_(timeout), last_progress_(NowNanoseconds()) {
        if (timeout_.count() > 0) {
            thread_ = std::thread([this] { Run(); });
        }
    }

    ~ProgressWatchdog() { Stop(); }

    ProgressWatchdog(const ProgressWatchdog&) = delete;
    ProgressWatchdog& operator=(const ProgressWatchdog&) = delete;

    void NotifyProgress() { last_progress_.store(NowNanoseconds(), std::memory_order_release); }

    [[nodiscard]] bool ShouldAbort() const noexcept {
        return aborted_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool Aborted() const noexcept {
        return ShouldAbort();
    }

    void Stop() noexcept {
        done_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

  private:
    void Run() noexcept {
        const auto tick = std::max(std::chrono::milliseconds(1),
                                   std::min(std::chrono::milliseconds(50), timeout_ / 4));
        const auto timeout_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(timeout_).count());
        while (!done_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(tick);
            if (done_.load(std::memory_order_acquire)) {
                break;
            }
            const std::uint64_t now = NowNanoseconds();
            const std::uint64_t last = last_progress_.load(std::memory_order_acquire);
            if (now >= last && now - last >= timeout_ns) {
                aborted_.store(true, std::memory_order_release);
                break;
            }
        }
    }

    std::chrono::milliseconds timeout_;
    std::atomic<bool> done_{false};
    std::atomic<bool> aborted_{false};
    std::atomic<std::uint64_t> last_progress_{0};
    std::thread thread_;
};

enum class TaskState : std::uint8_t {
    Pending,
    Running,
    Succeeded,
    Failed,
    Canceled,
};

struct DownloadTask {
    serialize::ManifestBundleView bundle;
    std::string bundle_name;
    std::string file_name;
    std::string temp_path;
    std::shared_ptr<SharedDownloadGate> gate;
    core::Error error{ErrorCode::Ok};
    TaskState state = TaskState::Pending;
    std::uint64_t downloaded = 0;
    int attempt = 0;
    int url_cursor = 0;
};

class BundleDownloader final : public ResourceDownloader {
  public:
    BundleDownloader(std::string package_name, std::string package_version, ManifestPtr manifest,
                     std::span<const std::uint32_t> bundle_indices,
                     std::shared_ptr<FileSystemHost> host,
                     std::shared_ptr<IFileSystem> download_file_system,
                     std::shared_ptr<IHttpTransport> transport,
                     std::shared_ptr<IDownloadUrlProvider> url_provider, DownloadOptions options)
        : package_name_(std::move(package_name)), package_version_(std::move(package_version)),
          manifest_(std::move(manifest)), file_system_(std::move(download_file_system)),
          transport_(std::move(transport)), url_provider_(std::move(url_provider)),
          options_(std::move(options)) {
        (void)host;
        tasks_.reserve(bundle_indices.size());
        for (const std::uint32_t index : bundle_indices) {
            if (!manifest_ || index >= manifest_->Bundles().size()) {
                continue;
            }
            DownloadTask task;
            task.bundle = manifest_->Bundles()[index];
            task.bundle_name.assign(task.bundle.bundle_name);
            task.file_name.assign(task.bundle.file_name);
            total_bytes_ += task.bundle.file_size;
            tasks_.push_back(std::move(task));
        }
        total_count_ = tasks_.size();
    }

    ~BundleDownloader() override {
        Cancel();
        JoinWorkers();
    }

    void Start() override {
        std::lock_guard lock(mutex_);
        if (started_ && !done_) {
            return;
        }
        if (done_) {
            return;
        }
        started_ = true;
        canceled_ = false;
        started_at_ = std::chrono::steady_clock::now();
        finished_at_ = {};
        frame_start_ = std::chrono::steady_clock::now();
        starts_this_frame_ = 0;
        if (tasks_.empty()) {
            done_ = true;
            finished_at_ = started_at_;
            return;
        }
        const int count = std::clamp(options_.max_concurrency, 1, static_cast<int>(tasks_.size()));
        active_workers_ = count;
        workers_.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    void Pause() override {
        std::lock_guard lock(mutex_);
        paused_ = true;
    }

    void Resume() override {
        std::lock_guard lock(mutex_);
        paused_ = false;
        condition_.notify_all();
    }

    void Cancel() override {
        {
            std::lock_guard lock(mutex_);
            if (done_) {
                canceled_ = true;
                return;
            }
            canceled_ = true;
            paused_ = false;
            for (auto& task : tasks_) {
                if (task.state == TaskState::Pending) {
                    task.state = TaskState::Canceled;
                    task.error = MakeError(ErrorCode::Canceled, "download was canceled")
                                     .WithBundle(task.bundle_name)
                                     .WithPath(task.file_name);
                    if (!task.temp_path.empty() && options_.remove_temp_on_cancel) {
                        core::PlatformFile platform;
                        (void)platform.Delete(task.temp_path);
                    }
                }
            }
            if (active_workers_ == 0 && AllTerminalNoLock()) {
                done_.store(true, std::memory_order_release);
                finished_at_ = std::chrono::steady_clock::now();
            }
            condition_.notify_all();
        }
        Emit(DownloadEventType::Canceled, nullptr);
    }

    void RetryFailed() override {
        if (!done_.load(std::memory_order_acquire)) {
            return;
        }
        JoinWorkers();
        {
            std::lock_guard lock(mutex_);
            bool changed = false;
            for (auto& task : tasks_) {
                if (task.state == TaskState::Failed) {
                    task.state = TaskState::Pending;
                    task.error = core::Error{ErrorCode::Ok};
                    task.attempt = 0;
                    task.url_cursor = 0;
                    changed = true;
                }
            }
            if (!changed) {
                return;
            }
            failed_error_ = core::Error{ErrorCode::Ok};
            done_ = false;
            started_ = false;
            canceled_ = false;
            next_task_ = 0;
            started_at_ = {};
            finished_at_ = {};
            frame_start_ = std::chrono::steady_clock::now();
            starts_this_frame_ = 0;
        }
        Start();
    }

    [[nodiscard]] bool IsDone() const override { return done_.load(std::memory_order_acquire); }

    [[nodiscard]] core::OperationStatus Status() const override {
        std::lock_guard lock(mutex_);
        if (canceled_) {
            return core::OperationStatus::Canceled;
        }
        if (!done_) {
            return core::OperationStatus::Running;
        }
        for (const auto& task : tasks_) {
            if (task.state == TaskState::Failed) {
                return core::OperationStatus::Failed;
            }
        }
        return core::OperationStatus::Succeeded;
    }

    [[nodiscard]] float Progress() const override {
        std::lock_guard lock(mutex_);
        if (total_bytes_ != 0) {
            return std::clamp(static_cast<float>(DownloadedBytesNoLock()) /
                                  static_cast<float>(total_bytes_),
                              0.0f, 1.0f);
        }
        if (total_count_ == 0) {
            return 1.0f;
        }
        return static_cast<float>(CompletedCountNoLock()) / static_cast<float>(total_count_);
    }

    [[nodiscard]] const core::Error& LastError() const override {
        std::lock_guard lock(mutex_);
        if (!failed_error_.IsOk()) {
            return failed_error_;
        }
        for (const auto& task : tasks_) {
            if (!task.error.IsOk()) {
                return task.error;
            }
        }
        return failed_error_;
    }

    [[nodiscard]] std::uint64_t DownloadedBytes() const override {
        std::lock_guard lock(mutex_);
        return DownloadedBytesNoLock();
    }

    [[nodiscard]] std::uint64_t TotalBytes() const override {
        std::lock_guard lock(mutex_);
        return total_bytes_;
    }

    [[nodiscard]] DownloadSummary Summary() const override {
        std::lock_guard lock(mutex_);
        DownloadSummary summary;
        summary.total_count = total_count_;
        summary.total_bytes = total_bytes_;
        summary.downloaded_bytes = DownloadedBytesNoLock();
        if (started_at_ != std::chrono::steady_clock::time_point{}) {
            const auto end = done_.load(std::memory_order_acquire)
                                 ? finished_at_
                                 : std::chrono::steady_clock::now();
            summary.elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(end - started_at_);
            const auto millis = std::max<std::int64_t>(1, summary.elapsed.count());
            summary.bytes_per_second = summary.downloaded_bytes * 1000 /
                                       static_cast<std::uint64_t>(millis);
        }
        for (const auto& task : tasks_) {
            if (task.state == TaskState::Succeeded) {
                ++summary.completed_count;
            } else if (task.state == TaskState::Failed) {
                ++summary.failed_count;
            } else if (task.state == TaskState::Running) {
                ++summary.active_count;
            }
        }
        return summary;
    }

    [[nodiscard]] std::vector<DownloadFailure> Failures() const override {
        std::lock_guard lock(mutex_);
        std::vector<DownloadFailure> failures;
        for (const auto& task : tasks_) {
            if (task.state == TaskState::Failed) {
                failures.push_back(DownloadFailure{task.bundle_name, task.file_name, task.error});
            }
        }
        return failures;
    }

    std::string_view PackageName() const noexcept { return package_name_; }
    std::string_view PackageVersion() const noexcept { return package_version_; }

  private:
    void JoinWorkers() {
        std::vector<std::thread> workers;
        {
            std::lock_guard lock(mutex_);
            workers.swap(workers_);
        }
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    [[nodiscard]] std::size_t CompletedCountNoLock() const noexcept {
        std::size_t count = 0;
        for (const auto& task : tasks_) {
            if (task.state == TaskState::Succeeded) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::uint64_t DownloadedBytesNoLock() const noexcept {
        std::uint64_t total = 0;
        for (const auto& task : tasks_) {
            total += task.downloaded;
        }
        return total;
    }

    [[nodiscard]] bool AllTerminalNoLock() const noexcept {
        return std::all_of(tasks_.begin(), tasks_.end(), [](const DownloadTask& task) {
            return task.state == TaskState::Succeeded || task.state == TaskState::Failed ||
                   task.state == TaskState::Canceled;
        });
    }

    void AdvanceToPendingNoLock() noexcept {
        while (next_task_ < tasks_.size() &&
               tasks_[next_task_].state != TaskState::Pending) {
            ++next_task_;
        }
    }

    [[nodiscard]] bool AcquireStartPermit(std::unique_lock<std::mutex>& lock) {
        const int quota = std::clamp(options_.max_requests_per_frame, 1, 32);
        const auto interval = options_.scheduler_frame_interval;
        if (interval.count() <= 0) {
            return true;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= frame_start_ + interval) {
            frame_start_ = now;
            starts_this_frame_ = 0;
        }
        while (starts_this_frame_ >= quota) {
            condition_.wait_until(lock, frame_start_ + interval, [this] {
                return canceled_ || done_ || paused_;
            });
            if (canceled_ || done_ || paused_) {
                return false;
            }
            now = std::chrono::steady_clock::now();
            if (now >= frame_start_ + interval) {
                frame_start_ = now;
                starts_this_frame_ = 0;
            }
        }
        ++starts_this_frame_;
        return true;
    }

    void WorkerLoop() {
        for (;;) {
            DownloadTask* task = nullptr;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] {
                    return canceled_ || done_ || (!paused_ && next_task_ < tasks_.size());
                });
                if (canceled_ || done_) {
                    break;
                }
                AdvanceToPendingNoLock();
                if (next_task_ >= tasks_.size()) {
                    break;
                }
                if (!AcquireStartPermit(lock)) {
                    continue;
                }
                AdvanceToPendingNoLock();
                if (next_task_ >= tasks_.size()) {
                    break;
                }
                task = &tasks_[next_task_++];
                task->state = TaskState::Running;
            }

            Emit(DownloadEventType::FileStarted, task);
            Process(*task);

            {
                std::lock_guard lock(mutex_);
                if (!canceled_ && AllTerminalNoLock()) {
                    done_.store(true, std::memory_order_release);
                    finished_at_ = std::chrono::steady_clock::now();
                }
                condition_.notify_all();
            }
            if (done_.load(std::memory_order_acquire)) {
                break;
            }
        }

        {
            std::lock_guard lock(mutex_);
            if (active_workers_ > 0) {
                --active_workers_;
            }
            if (AllTerminalNoLock()) {
                done_.store(true, std::memory_order_release);
                finished_at_ = std::chrono::steady_clock::now();
            }
            condition_.notify_all();
        }
    }

    [[nodiscard]] static bool IsRetryableStatus(int status) noexcept {
        if (status == 0 || status == 408 || status == 429) {
            return true;
        }
        return status >= 500 && status < 600;
    }

    [[nodiscard]] std::chrono::milliseconds RetryDelay(const DownloadTask& task) const {
        auto delay = options_.retry_base_delay;
        for (int i = 0; i < task.attempt && delay < options_.retry_max_delay; ++i) {
            delay *= 2;
            if (delay >= options_.retry_max_delay) {
                delay = options_.retry_max_delay;
                break;
            }
        }
        if (options_.retry_jitter && delay.count() > 0) {
            std::uint64_t seed =
                static_cast<std::uint64_t>(task.url_cursor) + 1469598103934665603ULL;
            for (const char c : task.file_name) {
                seed ^= static_cast<unsigned char>(c);
                seed *= 1099511628211ULL;
            }
            seed ^= static_cast<std::uint64_t>(task.attempt) * 1099511628211ULL;
            const auto jitter = static_cast<std::int64_t>(seed % 1000ULL);
            delay += std::chrono::milliseconds(jitter * delay.count() / 1000);
        }
        return std::min(delay, options_.retry_max_delay);
    }

    void WaitRetry(const DownloadTask& task) {
        const auto delay = RetryDelay(task);
        if (delay.count() <= 0) {
            return;
        }
        std::unique_lock lock(mutex_);
        condition_.wait_for(lock, delay,
                            [this] { return canceled_.load(std::memory_order_acquire); });
    }

    [[nodiscard]] Result<void> WriteBody(DownloadTask& task, std::span<const std::byte> body,
                                         bool append) {
        if (!append) {
            core::PlatformFile platform;
            auto exists = platform.Exists(task.temp_path);
            if (exists && exists.Value()) {
                PARCEL_TRY(platform.Delete(task.temp_path));
            }
        }
        const std::size_t slash = task.temp_path.find_last_of('/');
        core::PlatformFile platform;
        if (slash != std::string::npos) {
            PARCEL_TRY(platform.CreateDirectories(task.temp_path.substr(0, slash)));
        }
        PARCEL_TRY_DECL(std::unique_ptr<core::IFile>, file,
                        platform.OpenWrite(task.temp_path, append));
        PARCEL_TRY(core::WriteAll(*file, body));
        PARCEL_TRY(file->Flush());
        file->Close();
        return {};
    }

    void FinishTask(DownloadTask& task, TaskState state, core::Error error,
                    DownloadEventType event_type) {
        {
            std::lock_guard lock(mutex_);
            task.state = state;
            task.error = std::move(error);
            if (state == TaskState::Failed && failed_error_.IsOk()) {
                failed_error_ = task.error;
            }
            if (state == TaskState::Succeeded) {
                task.downloaded = task.bundle.file_size;
            }
            condition_.notify_all();
        }
        if (task.gate) {
            auto gate = std::move(task.gate);
            gate->Complete(task.error, task.downloaded, task.bundle.file_size);
            ReleaseSharedDownloadGate(task.temp_path, gate);
        }
        DownloadEvent event;
        event.type = event_type;
        event.bundle_name = task.bundle_name;
        event.file_name = task.file_name;
        event.downloaded_bytes = task.downloaded;
        event.total_bytes = task.bundle.file_size;
        event.attempt = task.attempt;
        event.error = task.error;
        Emit(std::move(event));
    }

    void Emit(DownloadEventType type, const DownloadTask* task) {
        if (!options_.on_event) {
            return;
        }
        DownloadEvent event;
        event.type = type;
        if (task != nullptr) {
            event.bundle_name = task->bundle_name;
            event.file_name = task->file_name;
            event.downloaded_bytes = task->downloaded;
            event.total_bytes = task->bundle.file_size;
            event.attempt = task->attempt;
            event.error = task->error;
        }
        Emit(std::move(event));
    }

    void Emit(DownloadEvent event) {
        if (!options_.on_event) {
            return;
        }
        std::lock_guard lock(event_mutex_);
        options_.on_event(event);
    }

    void Process(DownloadTask& task) {
        if (!manifest_ || !file_system_ || !transport_ || !url_provider_) {
            FinishTask(task, TaskState::Failed,
                       MakeError(ErrorCode::Internal, "downloader is not configured")
                           .WithBundle(task.bundle_name)
                           .WithPath(task.file_name),
                       DownloadEventType::FileFailed);
            return;
        }

        auto temp = file_system_->DownloadTempPath(task.bundle);
        if (!temp) {
            FinishTask(task, TaskState::Failed, std::move(temp).TakeError(),
                       DownloadEventType::FileFailed);
            return;
        }
        task.temp_path = std::move(temp).Value();
        auto gate = SharedDownloadGateFor(task.temp_path);
        bool owner = false;
        {
            std::unique_lock gate_lock(gate->mutex);
            if (!gate->claimed && !gate->done) {
                gate->claimed = true;
                owner = true;
            }
        }
        if (!owner) {
            std::unique_lock gate_lock(gate->mutex);
            gate->condition.wait(gate_lock, [&gate] { return gate->done; });
            core::Error wait_error = gate->error;
            const std::uint64_t downloaded = gate->downloaded;
            gate_lock.unlock();
            task.downloaded = downloaded;
            if (wait_error.IsOk()) {
                FinishTask(task, TaskState::Succeeded, core::Error{ErrorCode::Ok},
                           DownloadEventType::FileCompleted);
            } else if (wait_error.code == ErrorCode::Canceled) {
                FinishTask(task, TaskState::Canceled, std::move(wait_error),
                           DownloadEventType::FileFailed);
            } else {
                FinishTask(task, TaskState::Failed, std::move(wait_error),
                           DownloadEventType::FileFailed);
            }
            return;
        }
        task.gate = gate;
        if (!file_system_->IsDownloadRequired(task.bundle)) {
            FinishTask(task, TaskState::Succeeded, core::Error{ErrorCode::Ok},
                       DownloadEventType::FileCompleted);
            return;
        }

        std::uint64_t existing = 0;
        {
            std::lock_guard lock(mutex_);
            core::PlatformFile platform;
            auto exists = platform.Exists(task.temp_path);
            if (exists && exists.Value()) {
                auto size = platform.Size(task.temp_path);
                if (size) {
                    existing = size.Value();
                }
            }
        }
        if (task.bundle.file_size != 0 && existing > task.bundle.file_size) {
            core::PlatformFile platform;
            (void)platform.Delete(task.temp_path);
            existing = 0;
        }
        const bool resume_enabled =
            options_.resume_minimum_size > 0 && existing >= options_.resume_minimum_size &&
            (task.bundle.file_size == 0 || existing < task.bundle.file_size);
        if (!resume_enabled && existing != 0) {
            core::PlatformFile platform;
            (void)platform.Delete(task.temp_path);
            existing = 0;
        }

        bool auth_refreshed = false;
        int failures = 0;
        const int max_retries = std::max(0, options_.max_retries);
        while (true) {
            if (canceled_.load(std::memory_order_acquire)) {
                if (options_.remove_temp_on_cancel) {
                    core::PlatformFile platform;
                    (void)platform.Delete(task.temp_path);
                }
                FinishTask(task, TaskState::Canceled,
                           MakeError(ErrorCode::Canceled, "download was canceled")
                               .WithBundle(task.bundle_name)
                               .WithPath(task.file_name),
                           DownloadEventType::FileFailed);
                return;
            }

            DownloadUrlRequest request;
            request.package = package_name_;
            request.version = package_version_;
            request.bundle_name = task.bundle_name;
            request.file_name = task.file_name;
            request.file_size = task.bundle.file_size;
            request.attempt = failures;

            auto urls_result = url_provider_->GetUrls(request);
            if (!urls_result || urls_result.Value().empty()) {
                core::Error error =
                    urls_result ? MakeError(ErrorCode::NotFound, "no download URL is available")
                                : std::move(urls_result).TakeError();
                error.WithBundle(task.bundle_name).WithPath(task.file_name);
                if (failures < max_retries) {
                    ++failures;
                    ++task.attempt;
                    WaitRetry(task);
                    continue;
                }
                FinishTask(task, TaskState::Failed, std::move(error),
                           DownloadEventType::FileFailed);
                return;
            }
            const auto& urls = urls_result.Value();
            const std::string& url = urls[static_cast<std::size_t>(task.url_cursor) % urls.size()];

            auto headers_result = url_provider_->GetHeaders(request);
            if (!headers_result) {
                FinishTask(task, TaskState::Failed, std::move(headers_result).TakeError(),
                           DownloadEventType::FileFailed);
                return;
            }

            HttpRequest http_request;
            http_request.url = url;
            http_request.headers = std::move(headers_result).Value();
            if (resume_enabled && existing > 0) {
                http_request.range_offset = existing;
            }
            http_request.timeout = options_.timeout;
            http_request.watchdog_timeout = options_.watchdog_timeout;

            ProgressWatchdog watchdog(options_.watchdog_timeout);
            http_request.on_bytes_received = [&watchdog](std::uint64_t) {
                watchdog.NotifyProgress();
            };
            http_request.should_abort = [&watchdog] { return watchdog.ShouldAbort(); };
            auto response = transport_->Send(http_request);
            watchdog.Stop();
            if (watchdog.Aborted()) {
                auto error = MakeError(ErrorCode::Timeout, "download watchdog timed out")
                                 .WithBundle(task.bundle_name)
                                 .WithPath(task.file_name);
                if (failures < max_retries) {
                    ++failures;
                    ++task.attempt;
                    ++task.url_cursor;
                    Emit(DownloadEventType::RetryScheduled, &task);
                    WaitRetry(task);
                    continue;
                }
                url_provider_->OnResult(request, url, false, 0);
                FinishTask(task, TaskState::Failed, std::move(error),
                           DownloadEventType::FileFailed);
                return;
            }
            if (!response) {
                core::Error error = std::move(response).TakeError();
                url_provider_->OnResult(request, url, false, 0);
                if (failures < max_retries) {
                    ++failures;
                    ++task.attempt;
                    ++task.url_cursor;
                    WaitRetry(task);
                    continue;
                }
                error.WithBundle(task.bundle_name).WithPath(task.file_name);
                FinishTask(task, TaskState::Failed, std::move(error),
                           DownloadEventType::FileFailed);
                return;
            }

            HttpResponse result = std::move(response).Value();
            if (canceled_.load(std::memory_order_acquire)) {
                if (options_.remove_temp_on_cancel) {
                    core::PlatformFile platform;
                    (void)platform.Delete(task.temp_path);
                }
                FinishTask(task, TaskState::Canceled,
                           MakeError(ErrorCode::Canceled, "download was canceled")
                               .WithBundle(task.bundle_name)
                               .WithPath(task.file_name),
                           DownloadEventType::FileFailed);
                return;
            }
            const int status = result.status;
            if (!result.error.IsOk()) {
                url_provider_->OnResult(request, url, false, status);
                if (failures < max_retries) {
                    ++failures;
                    ++task.attempt;
                    ++task.url_cursor;
                    WaitRetry(task);
                    continue;
                }
                auto error = std::move(result.error);
                error.WithBundle(task.bundle_name).WithPath(task.file_name).WithHttpStatus(status);
                FinishTask(task, TaskState::Failed, std::move(error),
                           DownloadEventType::FileFailed);
                return;
            }

            if (status == 401 || status == 403) {
                url_provider_->OnResult(request, url, false, status);
                if (!auth_refreshed) {
                    auth_refreshed = true;
                    if (url_provider_->OnUnauthorized(request, url)) {
                        continue;
                    }
                }
                if (url_provider_->ShouldRetryStatus(status) && failures < max_retries) {
                    ++failures;
                    ++task.attempt;
                    ++task.url_cursor;
                    Emit(DownloadEventType::RetryScheduled, &task);
                    WaitRetry(task);
                    continue;
                }
                auto error = MakeError(ErrorCode::Http, "download request was not authorized")
                                 .WithBundle(task.bundle_name)
                                 .WithPath(task.file_name)
                                 .WithHttpStatus(status);
                FinishTask(task, TaskState::Failed, std::move(error),
                           DownloadEventType::FileFailed);
                return;
            }
            if (status == 416 && resume_enabled && existing > 0) {
                core::PlatformFile platform;
                (void)platform.Delete(task.temp_path);
                existing = 0;
                ++task.url_cursor;
                continue;
            }

            if (result.IsSuccess()) {
                const bool append = status == 206 && resume_enabled && existing > 0;
                if (options_.max_file_bytes != 0 && result.body.size() > options_.max_file_bytes) {
                    FinishTask(
                        task, TaskState::Failed,
                        MakeError(ErrorCode::InvalidArgument, "download exceeds the size limit")
                            .WithBundle(task.bundle_name)
                            .WithPath(task.file_name),
                        DownloadEventType::FileFailed);
                    return;
                }
                if (task.bundle.file_size > 0) {
                    const std::size_t slash = task.temp_path.find_last_of('/');
                    const std::string directory = slash == std::string::npos
                                                      ? task.temp_path
                                                      : task.temp_path.substr(0, slash);
                    core::PlatformFile platform;
                    auto free_space = platform.FreeSpace(directory);
                    const std::uint64_t required =
                        task.bundle.file_size > existing ? task.bundle.file_size - existing : 0;
                    if (free_space && free_space.Value() < required) {
                        FinishTask(task, TaskState::Failed,
                                   MakeError(ErrorCode::Io, "insufficient disk space")
                                       .WithBundle(task.bundle_name)
                                       .WithPath(task.file_name),
                                   DownloadEventType::FileFailed);
                        return;
                    }
                }
                auto written = WriteBody(task, result.body, append);
                if (!written) {
                    FinishTask(task, TaskState::Failed, std::move(written).TakeError(),
                               DownloadEventType::FileFailed);
                    return;
                }
                task.downloaded = append ? existing + result.body.size() : result.body.size();
                {
                    std::lock_guard lock(mutex_);
                    condition_.notify_all();
                }
                Emit(DownloadEventType::Progress, &task);
                if (canceled_.load(std::memory_order_acquire)) {
                    if (options_.remove_temp_on_cancel) {
                        core::PlatformFile platform;
                        (void)platform.Delete(task.temp_path);
                    }
                    FinishTask(task, TaskState::Canceled,
                               MakeError(ErrorCode::Canceled, "download was canceled")
                                   .WithBundle(task.bundle_name)
                                   .WithPath(task.file_name),
                               DownloadEventType::FileFailed);
                    return;
                }

                auto committed = file_system_->CommitDownloadedBundle(task.bundle, task.temp_path);
                if (committed) {
                    url_provider_->OnResult(request, url, true, status);
                    FinishTask(task, TaskState::Succeeded, core::Error{ErrorCode::Ok},
                               DownloadEventType::FileCompleted);
                    return;
                }
                core::Error commit_error = std::move(committed).TakeError();
                core::PlatformFile platform;
                (void)platform.Delete(task.temp_path);
                existing = 0;
                if (failures < max_retries) {
                    ++failures;
                    ++task.attempt;
                    ++task.url_cursor;
                    WaitRetry(task);
                    continue;
                }
                commit_error.WithBundle(task.bundle_name).WithPath(task.file_name);
                FinishTask(task, TaskState::Failed, std::move(commit_error),
                           DownloadEventType::FileFailed);
                return;
            }

            url_provider_->OnResult(request, url, false, status);
            core::Error error = MakeError(ErrorCode::Http, "download request failed")
                                    .WithBundle(task.bundle_name)
                                    .WithPath(task.file_name)
                                    .WithHttpStatus(status);
            if ((IsRetryableStatus(status) || url_provider_->ShouldRetryStatus(status)) &&
                failures < max_retries) {
                ++failures;
                ++task.attempt;
                ++task.url_cursor;
                Emit(DownloadEventType::RetryScheduled, &task);
                WaitRetry(task);
                continue;
            }
            FinishTask(task, TaskState::Failed, std::move(error), DownloadEventType::FileFailed);
            return;
        }
    }

    std::string package_name_;
    std::string package_version_;
    ManifestPtr manifest_;
    std::shared_ptr<IFileSystem> file_system_;
    std::shared_ptr<IHttpTransport> transport_;
    std::shared_ptr<IDownloadUrlProvider> url_provider_;
    DownloadOptions options_;

    mutable std::mutex mutex_;
    mutable std::mutex event_mutex_;
    std::condition_variable condition_;
    std::vector<std::thread> workers_;
    std::vector<DownloadTask> tasks_;
    core::Error failed_error_{ErrorCode::Ok};
    std::size_t total_count_ = 0;
    std::uint64_t total_bytes_ = 0;
    std::size_t next_task_ = 0;
    std::chrono::steady_clock::time_point started_at_{};
    std::chrono::steady_clock::time_point finished_at_{};
    std::chrono::steady_clock::time_point frame_start_{};
    int starts_this_frame_ = 0;
    int active_workers_ = 0;
    bool started_ = false;
    std::atomic<bool> done_{false};
    std::atomic<bool> canceled_{false};
    bool paused_ = false;
};

} // namespace

std::optional<std::string_view> HttpResponse::Header(std::string_view name) const noexcept {
    for (const auto& header : headers) {
        if (EqualsAsciiIgnoreCase(header.name, name)) {
            return header.value;
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> HttpResponse::ContentLength() const noexcept {
    const auto value = Header("content-length");
    if (!value) {
        return std::nullopt;
    }
    return ParseUnsigned(*value);
}

std::shared_ptr<ResourceDownloader> MakeBundleDownloader(
    std::string package_name, std::string package_version, ManifestPtr manifest,
    std::span<const std::uint32_t> bundle_indices, std::shared_ptr<FileSystemHost> host,
    std::shared_ptr<IFileSystem> download_file_system,
    std::shared_ptr<IHttpTransport> download_transport,
    std::shared_ptr<IDownloadUrlProvider> download_url_provider, DownloadOptions options) {
    return std::make_shared<BundleDownloader>(
        std::move(package_name), std::move(package_version), std::move(manifest), bundle_indices,
        std::move(host), std::move(download_file_system), std::move(download_transport),
        std::move(download_url_provider), std::move(options));
}

} // namespace parcel::runtime






