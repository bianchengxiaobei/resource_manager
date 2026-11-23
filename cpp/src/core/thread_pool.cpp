#include "parcel/core/thread_pool.hpp"

#include "parcel/core/log.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <exception>
#include <utility>

namespace parcel::core {

ThreadPool::ThreadPool(ThreadPoolOptions options) : options_(std::move(options)) {
    thread_count_ = options_.thread_count;
    if (thread_count_ == 0) {
        thread_count_ = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    }
    thread_count_ = std::min<std::size_t>(thread_count_, 64);

    workers_.reserve(thread_count_);
#if PARCEL_HAS_EXCEPTIONS
    try {
#endif
        for (std::size_t i = 0; i < thread_count_; ++i) {
            workers_.emplace_back([this, i] { WorkerLoop(i); });
        }
        timer_ = std::thread([this] { TimerLoop(); });
#if PARCEL_HAS_EXCEPTIONS
    } catch (...) {
        Stop(false);
        throw;
    }
#endif
}

ThreadPool::~ThreadPool() { Stop(true); }

bool ThreadPool::Post(std::function<void()> task) {
    if (!task) {
        return false;
    }

    if (stopping_.load(std::memory_order_acquire)) {
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        if (stopping_.load(std::memory_order_acquire)) {
            return false;
        }
        tasks_.push_back(std::move(task));
        pending_.fetch_add(1, std::memory_order_relaxed);
    }
    worker_cv_.notify_one();
    return true;
}

bool ThreadPool::Post(CancellationToken token, std::function<void(const CancellationToken&)> task) {
    if (!task) {
        return false;
    }
    return Post([token = std::move(token), task = std::move(task)] { task(token); });
}

bool ThreadPool::PostDelayed(std::chrono::milliseconds delay, std::function<void()> task) {
    if (!task) {
        return false;
    }
    if (delay <= std::chrono::milliseconds::zero()) {
        return Post(std::move(task));
    }

    const auto due = SteadyClock::now() + delay;
    {
        std::lock_guard lock(mutex_);
        if (stopping_.load(std::memory_order_acquire)) {
            return false;
        }
        delayed_.push_back(DelayedTask{due, next_delayed_sequence_++, std::move(task)});
        pending_.fetch_add(1, std::memory_order_relaxed);
    }
    timer_cv_.notify_one();
    return true;
}

bool ThreadPool::PostDelayed(std::chrono::milliseconds delay, CancellationToken token,
                             std::function<void(const CancellationToken&)> task) {
    if (!task) {
        return false;
    }
    return PostDelayed(delay, [token = std::move(token), task = std::move(task)] { task(token); });
}

void ThreadPool::Stop(bool drain) {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    {
        std::lock_guard lock(mutex_);
        pending_.fetch_sub(delayed_.size(), std::memory_order_relaxed);
        delayed_.clear();
        if (!drain) {
            pending_.fetch_sub(tasks_.size(), std::memory_order_relaxed);
            tasks_.clear();
        }
    }

    worker_cv_.notify_all();
    timer_cv_.notify_all();

    if (timer_.joinable()) {
        timer_.join();
    }
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::size_t ThreadPool::ThreadCount() const noexcept { return thread_count_; }

std::size_t ThreadPool::PendingTasks() const noexcept {
    return pending_.load(std::memory_order_relaxed);
}

void ThreadPool::WorkerLoop(std::size_t index) {
    SetThreadName(index);

    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            worker_cv_.wait(lock, [this] {
                return stopping_.load(std::memory_order_acquire) || !tasks_.empty();
            });

            if (tasks_.empty()) {
                if (stopping_.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        if (task) {
#if PARCEL_HAS_EXCEPTIONS
            try {
                task();
            } catch (const std::exception& exception) {
                Log::Write(LogLevel::Error, "thread_pool",
                           std::string("worker task threw: ") + exception.what());
            } catch (...) {
                Log::Write(LogLevel::Error, "thread_pool",
                           "worker task threw an unknown exception");
            }
#else
            task();
#endif
            pending_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

void ThreadPool::TimerLoop() {
    SetThreadName(thread_count_);

    std::unique_lock lock(mutex_);
    while (!stopping_.load(std::memory_order_acquire)) {
        if (delayed_.empty()) {
            timer_cv_.wait(lock, [this] {
                return stopping_.load(std::memory_order_acquire) || !delayed_.empty();
            });
            continue;
        }

        const auto earliest = std::min_element(
            delayed_.begin(), delayed_.end(), [](const DelayedTask& lhs, const DelayedTask& rhs) {
                return lhs.due < rhs.due || (lhs.due == rhs.due && lhs.sequence < rhs.sequence);
            });
        const auto due = earliest->due;
        if (SteadyClock::now() < due) {
            timer_cv_.wait_until(lock, due);
            continue;
        }

        while (!delayed_.empty()) {
            const auto next = std::min_element(
                delayed_.begin(), delayed_.end(),
                [](const DelayedTask& lhs, const DelayedTask& rhs) {
                    return lhs.due < rhs.due || (lhs.due == rhs.due && lhs.sequence < rhs.sequence);
                });
            if (next->due > SteadyClock::now()) {
                break;
            }
            DelayedTask task = std::move(*next);
            delayed_.erase(next);
            tasks_.push_back(std::move(task.task));
        }
        worker_cv_.notify_all();
    }
}

void ThreadPool::SetThreadName(std::size_t index) {
    const std::string name = options_.thread_name_prefix + "-" + std::to_string(index);
    const int needed = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    if (needed <= 0) {
        return;
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide.data(), needed);
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace parcel::core
