#pragma once

#include "parcel/core/cancellation.hpp"
#include "parcel/core/error.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace parcel::core {

enum class OperationStatus : std::uint8_t {
    None,
    Running,
    Succeeded,
    Failed,
    Canceled,
};

[[nodiscard]] constexpr std::string_view ToString(OperationStatus status) noexcept {
    switch (status) {
    case OperationStatus::None:
        return "None";
    case OperationStatus::Running:
        return "Running";
    case OperationStatus::Succeeded:
        return "Succeeded";
    case OperationStatus::Failed:
        return "Failed";
    case OperationStatus::Canceled:
        return "Canceled";
    }
    return "Unknown";
}

class OperationBase;
class OperationScheduler;

using OperationPtr = std::shared_ptr<OperationBase>;
using OperationCompletion = std::function<void(OperationBase&)>;

class OperationBase {
  public:
    OperationBase() = default;
    virtual ~OperationBase();

    OperationBase(const OperationBase&) = delete;
    OperationBase& operator=(const OperationBase&) = delete;

    void SetPriority(int priority) noexcept { priority_ = priority; }
    [[nodiscard]] int Priority() const noexcept { return priority_; }

    [[nodiscard]] OperationStatus Status() const noexcept { return status_; }
    [[nodiscard]] bool IsDone() const noexcept {
        return status_ == OperationStatus::Succeeded || status_ == OperationStatus::Failed ||
               status_ == OperationStatus::Canceled;
    }

    [[nodiscard]] float Progress() const noexcept;
    [[nodiscard]] std::chrono::milliseconds Elapsed() const noexcept;
    [[nodiscard]] const Error& LastError() const noexcept { return last_error_; }

    [[nodiscard]] CancellationToken Token() const noexcept { return cancellation_.Token(); }
    [[nodiscard]] bool IsCancellationRequested() const noexcept {
        return cancellation_.IsCancellationRequested();
    }

    void AddChild(OperationPtr child);
    [[nodiscard]] const std::vector<OperationPtr>& Children() const noexcept { return children_; }

    void OnComplete(OperationCompletion callback);

    void Start();
    void Tick();
    void Cancel();

    void InvokeCompletionCallbacks();

    void SetChildObserver(std::function<void(const OperationPtr&)> observer) {
        child_observer_ = std::move(observer);
    }

  protected:
    virtual void OnStart() {}
    virtual void OnTick() {}
    virtual void OnCancel() {}

    void SetProgress(float value) noexcept;
    void Succeed();
    void Fail(Error error);

  private:
    friend class OperationScheduler;

    void TryComplete();
    void Finalize(OperationStatus status, Error error);
    void OnChildCompleted(OperationBase& child);
    [[nodiscard]] bool AllChildrenDone() const noexcept;

    OperationStatus status_ = OperationStatus::None;
    OperationStatus requested_status_ = OperationStatus::Succeeded;
    Error last_error_{ErrorCode::Ok};
    Error requested_error_{ErrorCode::Ok};
    int priority_ = 0;
    std::chrono::steady_clock::time_point started_at_{};
    std::chrono::steady_clock::time_point finished_at_{};
    float progress_ = 0.0f;
    bool progress_explicit_ = false;
    bool completion_requested_ = false;
    bool callbacks_invoked_ = false;
    OperationBase* parent_ = nullptr;
    CancellationSource cancellation_;
    std::vector<OperationPtr> children_;
    std::vector<OperationCompletion> callbacks_;
    std::function<void(const OperationPtr&)> child_observer_;
};

} // namespace parcel::core

