#include "parcel/core/operation_scheduler.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace parcel::core {
namespace {
thread_local const OperationScheduler* tls_updating_scheduler = nullptr;
}

OperationScheduler::OperationScheduler(std::shared_ptr<IClock> clock,
                                       std::chrono::nanoseconds frame_budget)
    : clock_(clock ? std::move(clock) : std::make_shared<SystemClock>()),
      frame_budget_(std::max(frame_budget, std::chrono::nanoseconds::zero())) {}

OperationScheduler::~OperationScheduler() {
    for (auto& entry : entries_) {
        if (entry.operation) {
            entry.operation->SetChildObserver({});
        }
    }
}

void OperationScheduler::Add(OperationPtr operation) {
    if (!operation || operation->IsDone()) {
        return;
    }
    std::lock_guard lock(mutex_);
    AddRecursive(operation);
}

void OperationScheduler::Remove(const OperationPtr& operation) {
    if (!operation) {
        return;
    }
    std::lock_guard lock(mutex_);
    RemoveRecursive(operation);
}

void OperationScheduler::CancelAll() {
    std::vector<OperationPtr> operations;
    {
        std::lock_guard lock(mutex_);
        operations.reserve(entries_.size());
        for (const auto& entry : entries_) {
            if (entry.operation) {
                operations.push_back(entry.operation);
            }
        }
    }
    for (auto& operation : operations) {
        operation->Cancel();
    }
}

std::size_t OperationScheduler::Update() { return Update(frame_budget_); }

std::size_t OperationScheduler::Update(std::chrono::nanoseconds budget) {
    std::vector<OperationPtr> completed;
    std::size_t ticked = 0;
    {
        std::lock_guard lock(mutex_);
        if (in_update_.load(std::memory_order_acquire)) {
            return 0;
        }
        if (entries_.empty()) {
            return 0;
        }

        in_update_.store(true, std::memory_order_release);
        tls_updating_scheduler = this;
        SortEntries();
        RotateToCursor();

        const auto normalized_budget = std::max(budget, std::chrono::nanoseconds::zero());
        const auto deadline = clock_->Now() + normalized_budget;

        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const auto sequence = entries_[i].sequence;
            const OperationPtr operation = entries_[i].operation;

            if (operation && operation->Status() == OperationStatus::Running) {
                operation->Tick();
                ++ticked;
                cursor_sequence_ = sequence;
            }

            if (normalized_budget == std::chrono::nanoseconds::zero()) {
                break;
            }
            if (clock_->Now() >= deadline) {
                break;
            }
        }

        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->operation && it->operation->IsDone()) {
                it->operation->SetChildObserver({});
                completed.push_back(std::move(it->operation));
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }

        if (entries_.empty()) {
            cursor_sequence_ = 0;
        }

        completed_total_ += completed.size();
        tls_updating_scheduler = nullptr;
        in_update_.store(false, std::memory_order_release);
    }

    for (auto& operation : completed) {
        operation->InvokeCompletionCallbacks();
    }
    return ticked;
}

void OperationScheduler::SetFrameBudget(std::chrono::nanoseconds budget) noexcept {
    std::lock_guard lock(mutex_);
    frame_budget_ = std::max(budget, std::chrono::nanoseconds::zero());
}

std::chrono::nanoseconds OperationScheduler::FrameBudget() const noexcept {
    std::lock_guard lock(mutex_);
    return frame_budget_;
}

std::size_t OperationScheduler::ActiveCount() const noexcept {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

std::size_t OperationScheduler::CompletedCount() const noexcept {
    std::lock_guard lock(mutex_);
    return completed_total_;
}

Result<void> OperationScheduler::WaitForCompletion(OperationScheduler& scheduler,
                                                   const OperationPtr& operation,
                                                   std::chrono::milliseconds timeout) {
    if (!operation) {
        return MakeError(ErrorCode::InvalidArgument, "operation is null");
    }
    if (tls_updating_scheduler == &scheduler) {
        return MakeError(ErrorCode::Internal,
                         "cannot wait for an operation while the scheduler is updating");
    }

    if (!operation->IsDone()) {
        scheduler.Add(operation);
    }

    const bool finite = timeout > std::chrono::milliseconds::zero();
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (!operation->IsDone()) {
        if (scheduler.IsUpdating()) {
            std::this_thread::yield();
        } else {
            (void)scheduler.Update();
        }
        if (finite && std::chrono::steady_clock::now() >= deadline) {
            return MakeError(ErrorCode::Timeout, "operation did not complete before the timeout")
                .WithAsset("operation");
        }
        std::this_thread::yield();
    }

    if (operation->Status() == OperationStatus::Failed) {
        return operation->LastError();
    }
    if (operation->Status() == OperationStatus::Canceled) {
        return MakeError(ErrorCode::Canceled, "operation was canceled");
    }
    return {};
}

void OperationScheduler::AddRecursive(const OperationPtr& operation) {
    if (!operation || operation->IsDone() || scheduled_.contains(operation.get())) {
        return;
    }

    operation->SetChildObserver([this](const OperationPtr& child) { AddRecursive(child); });

    entries_.push_back(Entry{operation, next_sequence_++});
    scheduled_.insert(operation.get());

    for (const auto& child : operation->Children()) {
        AddRecursive(child);
    }

    operation->Start();
}

void OperationScheduler::RemoveRecursive(const OperationPtr& operation) {
    if (!operation) {
        return;
    }

    operation->SetChildObserver({});
    scheduled_.erase(operation.get());
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&operation](const Entry& entry) {
                                      return entry.operation.get() == operation.get();
                                  }),
                   entries_.end());

    for (const auto& child : operation->Children()) {
        RemoveRecursive(child);
    }
}

void OperationScheduler::SortEntries() {
    std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& lhs, const Entry& rhs) {
        if (lhs.operation->Priority() != rhs.operation->Priority()) {
            return lhs.operation->Priority() > rhs.operation->Priority();
        }
        return lhs.sequence < rhs.sequence;
    });
}

void OperationScheduler::RotateToCursor() {
    if (cursor_sequence_ == 0 || entries_.size() < 2) {
        return;
    }

    auto it = std::find_if(entries_.begin(), entries_.end(), [this](const Entry& entry) {
        return entry.sequence > cursor_sequence_;
    });
    if (it == entries_.end()) {
        it = entries_.begin();
    }
    std::rotate(entries_.begin(), it, entries_.end());
}

} // namespace parcel::core
