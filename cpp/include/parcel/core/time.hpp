#pragma once

#include <chrono>
#include <mutex>
#include <string>

namespace parcel::core {

using SteadyClock = std::chrono::steady_clock;
using SteadyTimePoint = SteadyClock::time_point;

class IClock {
  public:
    virtual ~IClock() = default;
    [[nodiscard]] virtual SteadyTimePoint Now() const = 0;
};

class SystemClock final : public IClock {
  public:
    [[nodiscard]] SteadyTimePoint Now() const override { return SteadyClock::now(); }
};

class FakeClock final : public IClock {
  public:
    FakeClock() = default;
    explicit FakeClock(SteadyTimePoint start) : now_(start) {}

    [[nodiscard]] SteadyTimePoint Now() const override {
        std::lock_guard lock(mutex_);
        return now_;
    }

    void SetNow(SteadyTimePoint value) {
        std::lock_guard lock(mutex_);
        now_ = value;
    }

    void Advance(std::chrono::nanoseconds delta) {
        std::lock_guard lock(mutex_);
        now_ += delta;
    }

  private:
    mutable std::mutex mutex_;
    SteadyTimePoint now_{};
};

[[nodiscard]] std::string FormatUtcTimestamp(std::chrono::system_clock::time_point value);

} // namespace parcel::core
