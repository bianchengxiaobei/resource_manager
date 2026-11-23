#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace parcel::core {

class CancellationSource;

class CancellationToken {
  public:
    CancellationToken() = default;

    [[nodiscard]] bool IsCancellationRequested() const noexcept {
        return flag_ != nullptr && flag_->load(std::memory_order_acquire);
    }

  private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<const std::atomic_bool> flag)
        : flag_(std::move(flag)) {}

    std::shared_ptr<const std::atomic_bool> flag_;
};

class CancellationSource {
  public:
    CancellationSource() : flag_(std::make_shared<std::atomic_bool>(false)) {}

    [[nodiscard]] CancellationToken Token() const noexcept { return CancellationToken(flag_); }

    void Cancel() noexcept { flag_->store(true, std::memory_order_release); }

    [[nodiscard]] bool IsCancellationRequested() const noexcept {
        return flag_->load(std::memory_order_acquire);
    }

  private:
    std::shared_ptr<std::atomic_bool> flag_;
};

} // namespace parcel::core
