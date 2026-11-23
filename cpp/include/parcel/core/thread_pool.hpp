#pragma once

#include "parcel/core/cancellation.hpp"
#include "parcel/core/config.hpp"
#include "parcel/core/time.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace parcel::core {

struct ThreadPoolOptions {
    std::size_t thread_count = 0;
    std::string thread_name_prefix = "parcel";
};

class IThreadPool {
  public:
    virtual ~IThreadPool() = default;

    [[nodiscard]] virtual bool Post(std::function<void()> task) = 0;
    [[nodiscard]] virtual bool Post(CancellationToken token,
                                    std::function<void(const CancellationToken&)> task) = 0;
    [[nodiscard]] virtual bool PostDelayed(std::chrono::milliseconds delay,
                                           std::function<void()> task) = 0;
    [[nodiscard]] virtual bool PostDelayed(std::chrono::milliseconds delay, CancellationToken token,
                                           std::function<void(const CancellationToken&)> task) = 0;

    virtual void Stop(bool drain = true) = 0;
    [[nodiscard]] virtual std::size_t ThreadCount() const noexcept = 0;
    [[nodiscard]] virtual std::size_t PendingTasks() const noexcept = 0;
};

class ThreadPool final : public IThreadPool {
  public:
    explicit ThreadPool(ThreadPoolOptions options = {});
    ~ThreadPool() override;

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    [[nodiscard]] bool Post(std::function<void()> task) override;
    [[nodiscard]] bool Post(CancellationToken token,
                            std::function<void(const CancellationToken&)> task) override;
    [[nodiscard]] bool PostDelayed(std::chrono::milliseconds delay,
                                   std::function<void()> task) override;
    [[nodiscard]] bool PostDelayed(std::chrono::milliseconds delay, CancellationToken token,
                                   std::function<void(const CancellationToken&)> task) override;

    void Stop(bool drain = true) override;
    [[nodiscard]] std::size_t ThreadCount() const noexcept override;
    [[nodiscard]] std::size_t PendingTasks() const noexcept override;

  private:
    struct DelayedTask {
        SteadyTimePoint due;
        std::uint64_t sequence = 0;
        std::function<void()> task;
    };

    void WorkerLoop(std::size_t index);
    void TimerLoop();
    void SetThreadName(std::size_t index);

    ThreadPoolOptions options_;
    std::size_t thread_count_ = 0;
    std::vector<std::thread> workers_;
    std::thread timer_;

    mutable std::mutex mutex_;
    std::condition_variable worker_cv_;
    std::condition_variable timer_cv_;
    std::deque<std::function<void()>> tasks_;
    std::vector<DelayedTask> delayed_;
    std::uint64_t next_delayed_sequence_ = 0;
    std::atomic_bool stopping_{false};
    std::atomic<std::size_t> pending_{0};
};

} // namespace parcel::core
