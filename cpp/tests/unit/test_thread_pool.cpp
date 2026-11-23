#include <catch_amalgamated.hpp>

#include "parcel/core/cancellation.hpp"
#include "parcel/core/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

using parcel::core::CancellationSource;
using parcel::core::ThreadPool;
using parcel::core::ThreadPoolOptions;

namespace {

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

} // namespace

TEST_CASE("ThreadPool executes posted tasks", "[core][thread_pool]") {
    ThreadPoolOptions options;
    options.thread_count = 4;
    options.thread_name_prefix = "test-pool";
    ThreadPool pool(options);

    REQUIRE(pool.ThreadCount() == 4);

    std::atomic<int> count{0};
    for (int i = 0; i < 200; ++i) {
        REQUIRE(pool.Post([&count] { count.fetch_add(1, std::memory_order_relaxed); }));
    }

    REQUIRE(WaitFor([&count] { return count.load(std::memory_order_relaxed) == 200; }));
    REQUIRE(WaitFor([&pool] { return pool.PendingTasks() == 0; }));
}

TEST_CASE("ThreadPool runs delayed tasks", "[core][thread_pool]") {
    ThreadPoolOptions options;
    options.thread_count = 2;
    ThreadPool pool(options);

    std::atomic_bool fired{false};
    REQUIRE(pool.PostDelayed(std::chrono::milliseconds(20),
                             [&fired] { fired.store(true, std::memory_order_release); }));

    REQUIRE(WaitFor([&fired] { return fired.load(std::memory_order_acquire); }));
    REQUIRE(WaitFor([&pool] { return pool.PendingTasks() == 0; }));
}

TEST_CASE("ThreadPool tasks observe cancellation tokens", "[core][thread_pool]") {
    ThreadPoolOptions options;
    options.thread_count = 2;
    ThreadPool pool(options);

    CancellationSource source;
    source.Cancel();

    std::atomic_bool observed{false};
    REQUIRE(pool.Post(source.Token(), [&observed](const auto& token) {
        observed.store(token.IsCancellationRequested(), std::memory_order_release);
    }));

    REQUIRE(WaitFor([&observed] { return observed.load(std::memory_order_acquire); }));
    REQUIRE(observed.load(std::memory_order_acquire));
}

#if PARCEL_HAS_EXCEPTIONS
TEST_CASE("ThreadPool isolates task exceptions", "[core][thread_pool]") {
    ThreadPoolOptions options;
    options.thread_count = 1;
    ThreadPool pool(options);

    std::atomic_bool continued{false};
    REQUIRE(pool.Post([] { throw std::runtime_error("intentional"); }));
    REQUIRE(pool.Post([&continued] { continued.store(true, std::memory_order_release); }));

    REQUIRE(WaitFor([&continued] { return continued.load(std::memory_order_acquire); }));
}
#endif

TEST_CASE("ThreadPool Stop drains or discards pending tasks", "[core][thread_pool]") {
    ThreadPoolOptions options;
    options.thread_count = 1;
    ThreadPool pool(options);

    std::atomic<int> ran{0};
    for (int i = 0; i < 20; ++i) {
        REQUIRE(pool.Post([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
    }
    pool.Stop(true);
    REQUIRE(ran.load(std::memory_order_relaxed) == 20);
    REQUIRE(pool.PendingTasks() == 0);
    REQUIRE_FALSE(pool.Post([] {}));

    ThreadPoolOptions discard_options;
    discard_options.thread_count = 1;
    ThreadPool discarded(discard_options);
    for (int i = 0; i < 20; ++i) {
        REQUIRE(discarded.Post([] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }));
    }
    discarded.Stop(false);
    REQUIRE(discarded.PendingTasks() == 0);
}
