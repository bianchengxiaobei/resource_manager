#include <catch_amalgamated.hpp>

#include "parcel/core/operation_scheduler.hpp"

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using parcel::core::Error;
using parcel::core::ErrorCode;
using parcel::core::FakeClock;
using parcel::core::OperationBase;
using parcel::core::OperationScheduler;
using parcel::core::OperationStatus;

namespace {

class TickOperation final : public OperationBase {
  public:
    explicit TickOperation(int ticks_to_finish, int priority = 0)
        : ticks_to_finish_(ticks_to_finish) {
        SetPriority(priority);
    }

    int Ticks() const noexcept { return ticks_; }

  protected:
    void OnTick() override {
        ++ticks_;
        SetProgress(static_cast<float>(ticks_) / static_cast<float>(ticks_to_finish_));
        if (ticks_ >= ticks_to_finish_) {
            Succeed();
        }
    }

  private:
    int ticks_ = 0;
    int ticks_to_finish_ = 1;
};

class ImmediateOperation final : public OperationBase {
  protected:
    void OnStart() override { Succeed(); }
};

class FailOperation final : public OperationBase {
  protected:
    void OnStart() override { Fail(Error(ErrorCode::Io, "intentional failure").WithAsset("test")); }
};

class LongOperation final : public OperationBase {
  public:
    int Ticks() const noexcept { return ticks_; }

  protected:
    void OnTick() override { ++ticks_; }

  private:
    int ticks_ = 0;
};

class ClockOperation final : public OperationBase {
  public:
    ClockOperation(FakeClock& clock, std::chrono::milliseconds step, int ticks_to_finish)
        : clock_(clock), step_(step), ticks_to_finish_(ticks_to_finish) {}

    int Ticks() const noexcept { return ticks_; }

  protected:
    void OnTick() override {
        ++ticks_;
        clock_.Advance(step_);
        if (ticks_ >= ticks_to_finish_) {
            Succeed();
        }
    }

  private:
    FakeClock& clock_;
    std::chrono::milliseconds step_;
    int ticks_to_finish_ = 1;
    int ticks_ = 0;
};

} // namespace

TEST_CASE("Operation completes over multiple ticks and invokes callback", "[core][operation]") {
    auto clock = std::make_shared<FakeClock>();
    OperationScheduler scheduler(clock);
    auto operation = std::make_shared<TickOperation>(2);

    bool completed = false;
    operation->OnComplete([&completed](OperationBase&) { completed = true; });

    scheduler.Add(operation);
    REQUIRE(operation->Status() == OperationStatus::Running);
    REQUIRE(operation->Progress() == 0.0f);

    (void)scheduler.Update();
    REQUIRE(operation->Status() == OperationStatus::Running);
    REQUIRE(operation->Progress() == Catch::Approx(0.5f));
    REQUIRE_FALSE(completed);

    (void)scheduler.Update();
    REQUIRE(operation->Status() == OperationStatus::Succeeded);
    REQUIRE(operation->Progress() == 1.0f);
    REQUIRE(completed);
    REQUIRE(scheduler.CompletedCount() == 1);
}

TEST_CASE("operation elapsed time starts on start and freezes on completion", "[core][operation]") {
    auto operation = std::make_shared<LongOperation>();
    REQUIRE(operation->Elapsed().count() == 0);

    operation->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto running = operation->Elapsed();
    REQUIRE(running.count() >= 1);

    operation->Cancel();
    const auto finished = operation->Elapsed();
    REQUIRE(finished >= running);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(operation->Elapsed() == finished);
}

TEST_CASE("Completion callbacks run only once", "[core][operation]") {
    auto clock = std::make_shared<FakeClock>();
    OperationScheduler scheduler(clock);
    auto operation = std::make_shared<ImmediateOperation>();

    int calls = 0;
    operation->OnComplete([&calls](OperationBase&) { ++calls; });

    scheduler.Add(operation);
    (void)scheduler.Update();
    (void)scheduler.Update();
    (void)scheduler.Update();

    REQUIRE(calls == 1);
}

TEST_CASE("Operation failure preserves error context", "[core][operation]") {
    auto clock = std::make_shared<FakeClock>();
    OperationScheduler scheduler(clock);
    auto operation = std::make_shared<FailOperation>();

    bool completed = false;
    operation->OnComplete([&completed](OperationBase&) { completed = true; });
    scheduler.Add(operation);
    (void)scheduler.Update();

    REQUIRE(operation->Status() == OperationStatus::Failed);
    REQUIRE(operation->LastError().code == ErrorCode::Io);
    REQUIRE(operation->LastError().asset == "test");
    REQUIRE(completed);
}

TEST_CASE("Parent waits for children and propagates success", "[core][operation]") {
    auto clock = std::make_shared<FakeClock>();
    OperationScheduler scheduler(clock);

    auto parent = std::make_shared<ImmediateOperation>();
    auto child = std::make_shared<TickOperation>(1);
    parent->AddChild(child);

    scheduler.Add(parent);
    REQUIRE(parent->Status() == OperationStatus::Running);
    REQUIRE(child->Status() == OperationStatus::Running);

    (void)scheduler.Update();
    REQUIRE(child->Status() == OperationStatus::Succeeded);
    REQUIRE(parent->Status() == OperationStatus::Succeeded);
}

TEST_CASE("Child failure fails the parent", "[core][operation]") {
    auto clock = std::make_shared<FakeClock>();
    OperationScheduler scheduler(clock);

    auto parent = std::make_shared<ImmediateOperation>();
    auto child = std::make_shared<FailOperation>();
    parent->AddChild(child);

    scheduler.Add(parent);
    (void)scheduler.Update();

    REQUIRE(child->Status() == OperationStatus::Failed);
    REQUIRE(parent->Status() == OperationStatus::Failed);
    REQUIRE(parent->LastError().code == ErrorCode::Io);
}

TEST_CASE("Cancel propagates through children", "[core][operation]") {
    auto clock = std::make_shared<FakeClock>();
    OperationScheduler scheduler(clock);

    auto parent = std::make_shared<OperationBase>();
    auto child = std::make_shared<LongOperation>();
    parent->AddChild(child);

    scheduler.Add(parent);
    (void)scheduler.Update();
    REQUIRE(child->Ticks() > 0);

    parent->Cancel();
    REQUIRE(parent->Status() == OperationStatus::Canceled);
    REQUIRE(child->Status() == OperationStatus::Canceled);
}

TEST_CASE("Scheduler honors priority and round-robin cursor", "[core][operation]") {
    auto clock = std::make_shared<FakeClock>();
    OperationScheduler scheduler(clock);

    auto high = std::make_shared<TickOperation>(1, 10);
    auto low = std::make_shared<TickOperation>(1, 0);
    scheduler.Add(high);
    scheduler.Add(low);

    (void)scheduler.Update(std::chrono::nanoseconds::zero());
    REQUIRE(high->Status() == OperationStatus::Succeeded);
    REQUIRE(low->Status() == OperationStatus::Running);

    (void)scheduler.Update(std::chrono::nanoseconds::zero());
    REQUIRE(low->Status() == OperationStatus::Succeeded);
}

TEST_CASE("Scheduler budget interrupts and resumes work", "[core][operation]") {
    auto clock = std::make_shared<FakeClock>();
    OperationScheduler scheduler(clock, std::chrono::milliseconds(1));

    auto first = std::make_shared<ClockOperation>(*clock, std::chrono::milliseconds(5), 3);
    auto second = std::make_shared<ClockOperation>(*clock, std::chrono::milliseconds(5), 3);
    scheduler.Add(first);
    scheduler.Add(second);

    (void)scheduler.Update();
    REQUIRE(first->Ticks() == 1);
    REQUIRE(second->Ticks() == 0);

    (void)scheduler.Update();
    REQUIRE(second->Ticks() == 1);
    REQUIRE(first->Ticks() == 1);
}

TEST_CASE("Completion callbacks can schedule new operations", "[core][operation]") {
    auto clock = std::make_shared<FakeClock>();
    OperationScheduler scheduler(clock);

    auto first = std::make_shared<ImmediateOperation>();
    auto second = std::make_shared<ImmediateOperation>();
    bool second_completed = false;

    first->OnComplete([&scheduler, &second, &second_completed](OperationBase&) {
        second->OnComplete([&second_completed](OperationBase&) { second_completed = true; });
        scheduler.Add(second);
    });

    scheduler.Add(first);
    (void)scheduler.Update();
    REQUIRE(first->Status() == OperationStatus::Succeeded);
    REQUIRE(second->Status() == OperationStatus::Succeeded);
    REQUIRE_FALSE(second_completed);

    (void)scheduler.Update();
    REQUIRE(second_completed);
}

TEST_CASE("Operation rejects cycles and duplicate parents", "[core][operation]") {
    auto parent = std::make_shared<ImmediateOperation>();
    auto child = std::make_shared<LongOperation>();

    parent->AddChild(child);
    child->AddChild(parent);

    REQUIRE(parent->Children().size() == 1);
    REQUIRE(child->Children().empty());
}

TEST_CASE("WaitForCompletion drives the scheduler", "[core][operation]") {
    auto clock = std::make_shared<FakeClock>();
    OperationScheduler scheduler(clock);

    auto operation = std::make_shared<TickOperation>(3);
    const auto result =
        OperationScheduler::WaitForCompletion(scheduler, operation, std::chrono::milliseconds(5000));

    REQUIRE(result.HasValue());
    REQUIRE(operation->Status() == OperationStatus::Succeeded);
}

