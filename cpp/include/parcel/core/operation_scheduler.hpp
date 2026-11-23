#pragma once

#include "parcel/core/operation.hpp"
#include "parcel/core/result.hpp"
#include "parcel/core/time.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace parcel::core {

class OperationScheduler {
  public:
    explicit OperationScheduler(
        std::shared_ptr<IClock> clock = nullptr,
        std::chrono::nanoseconds frame_budget = std::chrono::milliseconds(4));
    ~OperationScheduler();

    OperationScheduler(const OperationScheduler&) = delete;
    OperationScheduler& operator=(const OperationScheduler&) = delete;

    void Add(OperationPtr operation);
    void Remove(const OperationPtr& operation);
    void CancelAll();

    [[nodiscard]] std::size_t Update();
    [[nodiscard]] std::size_t Update(std::chrono::nanoseconds budget);

    void SetFrameBudget(std::chrono::nanoseconds budget) noexcept;
    [[nodiscard]] std::chrono::nanoseconds FrameBudget() const noexcept;

    [[nodiscard]] std::size_t ActiveCount() const noexcept;
    [[nodiscard]] std::size_t CompletedCount() const noexcept;
    [[nodiscard]] bool IsUpdating() const noexcept { return in_update_.load(); }

    static Result<void>
    WaitForCompletion(OperationScheduler& scheduler, const OperationPtr& operation,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

  private:
    struct Entry {
        OperationPtr operation;
        std::uint64_t sequence = 0;
    };

    void AddRecursive(const OperationPtr& operation);
    void RemoveRecursive(const OperationPtr& operation);
    void SortEntries();
    void RotateToCursor();

    mutable std::recursive_mutex mutex_;
    std::shared_ptr<IClock> clock_;
    std::chrono::nanoseconds frame_budget_;
    std::vector<Entry> entries_;
    std::unordered_set<const OperationBase*> scheduled_;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t cursor_sequence_ = 0;
    std::size_t completed_total_ = 0;
    std::atomic<bool> in_update_{false};
};

} // namespace parcel::core
