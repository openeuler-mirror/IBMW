// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for the time manipulator executor:
// EXT-016: invalid dt / wheel sizes are rejected at initialization.
// EXT-018: time ratios are validated and rounded.
// EXT-017: virtual time is in the system clock domain, time points before
// the start are due at once, and tasks on every wheel level (and beyond)
// run at their tick.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include "cpp/util/string.h"
#include "time_manipulator_ext/time_manipulator_executor.h"

namespace ibmw::extensions::time_manipulator_extension {
namespace {

using namespace std::chrono_literals;

// Runs every task on the calling thread.
class InlineExecutor {
 public:
  executor::ExecutorHandle Handle() { return executor::ExecutorHandle(&base_); }

 private:
  ibmw_executor_base_t base_{
      .type = [](void*) { return ibmw::util::ToIbmwStringView("inline"); },
      .name = [](void*) { return ibmw::util::ToIbmwStringView("inline"); },
      .is_thread_safe = [](void*) { return true; },
      .is_in_current_executor = [](void*) { return false; },
      .supports_timed_dispatch = [](void*) { return false; },
      .dispatch = [](void*, ibmw_function_base_t* task) { executor::Task{task}(); },
      .now = [](void*) -> uint64_t { return 0; },
      .dispatch_at_ns = [](void*, uint64_t, ibmw_function_base_t* task) { executor::Task{task}(); },
      .ctx = nullptr};
};

class TimeManipulatorExecutorTest : public ::testing::Test {
 protected:
  void Init(const std::string& options) {
    executor_.RegisterGetExecutorFunc([this](std::string_view) { return inline_.Handle(); });
    executor_.Initialize("tm", YAML::Load(options));
  }

  void TearDown() override { executor_.Shutdown(); }

  InlineExecutor inline_;
  TimeManipulatorExecutor executor_;
};

TEST(TimeManipulatorExecutorInit, RejectsInvalidDtAndWheelSizes) {
  for (const char* options : {"{bind_executor: x, dt_us: 0}", "{bind_executor: x, wheel_size: []}",
                              "{bind_executor: x, wheel_size: [4, 1]}", "{bind_executor: x, wheel_size: [0]}"}) {
    InlineExecutor inline_executor;
    TimeManipulatorExecutor executor;
    executor.RegisterGetExecutorFunc([&](std::string_view) { return inline_executor.Handle(); });
    EXPECT_ANY_THROW(executor.Initialize("tm", YAML::Load(options))) << options;
  }
}

// EXT-018: NaN, infinity and ratios above max_ratio are rejected (casting
// them to uint32_t is undefined behavior); ratios round to the nearest step.
TEST(TimeManipulatorExecutorInit, RejectsInvalidRatios) {
  for (const char* options : {"{bind_executor: x, init_ratio: .nan}", "{bind_executor: x, init_ratio: .inf}",
                              "{bind_executor: x, init_ratio: 1e12}", "{bind_executor: x, init_ratio: 1001}",
                              "{bind_executor: x, max_ratio: 0.5}", "{bind_executor: x, max_ratio: .inf}"}) {
    InlineExecutor inline_executor;
    TimeManipulatorExecutor executor;
    executor.RegisterGetExecutorFunc([&](std::string_view) { return inline_executor.Handle(); });
    EXPECT_ANY_THROW(executor.Initialize("tm", YAML::Load(options))) << options;
  }
}

TEST_F(TimeManipulatorExecutorTest, SetTimeRatioValidatesAndRounds) {
  Init("{bind_executor: x, init_ratio: 4}");
  EXPECT_DOUBLE_EQ(executor_.GetTimeRatio(), 4.0);
  for (double bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                     -std::numeric_limits<double>::infinity(), 1e12, 1000.5, 5e9}) {
    EXPECT_FALSE(executor_.SetTimeRatio(bad)) << bad;
    EXPECT_DOUBLE_EQ(executor_.GetTimeRatio(), 4.0) << bad;
  }
  const std::pair<double, double> cases[] = {
      {1000.0, 1000.0}, {1.9, 2.0}, {1.4, 1.0}, {1.0, 1.0}, {0.3, 1.0 / 3}, {0.25, 0.25},
      {0.0, 0.0}, {-1.0, 0.0}, {1e-20, 0.0}};
  for (auto [ratio, applied] : cases) {
    EXPECT_TRUE(executor_.SetTimeRatio(ratio)) << ratio;
    EXPECT_DOUBLE_EQ(executor_.GetTimeRatio(), applied) << ratio;
  }
}

// EXT-019: a paused timer reacts at once instead of within its next second.
TEST_F(TimeManipulatorExecutorTest, PauseEndsAtOnceOnResumeAndShutdown) {
  Init("{bind_executor: x, dt_us: 1000}");
  executor_.Start();
  ASSERT_TRUE(executor_.SetTimeRatio(0.0));
  std::this_thread::sleep_for(100ms);  // the timer thread is paused by now

  const auto paused_now = executor_.Now();
  auto begin = std::chrono::steady_clock::now();
  ASSERT_TRUE(executor_.SetTimeRatio(1.0));
  while (executor_.Now() == paused_now && std::chrono::steady_clock::now() - begin < 2s) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 500ms);

  ASSERT_TRUE(executor_.SetTimeRatio(0.0));
  std::this_thread::sleep_for(100ms);
  begin = std::chrono::steady_clock::now();
  executor_.Shutdown();
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 500ms);
}

TEST_F(TimeManipulatorExecutorTest, NowIsInTheSystemClockDomain) {
  Init("{bind_executor: x, dt_us: 1000, use_system_clock: false}");
  executor_.Start();
  const auto diff = executor_.Now() - std::chrono::system_clock::now();
  EXPECT_LT(std::chrono::abs(diff), 1s);
}

TEST_F(TimeManipulatorExecutorTest, TimePointBeforeStartRunsAtOnce) {
  Init("{bind_executor: x, dt_us: 1000}");
  executor_.Start();
  bool ran = false;
  executor_.DispatchAt(std::chrono::system_clock::time_point(1s), [&ran] { ran = true; });
  EXPECT_TRUE(ran);
}

TEST_F(TimeManipulatorExecutorTest, DispatchAfterShutdownIsDropped) {
  Init("{bind_executor: x, dt_us: 1000}");
  executor_.Start();
  executor_.Shutdown();
  bool ran = false;
  executor_.DispatchAt(executor_.Now() + 1h, [&ran] { ran = true; });
  EXPECT_FALSE(ran);
}

// Wheels of 4 and 4 ticks: targets land on level 0, level 1 and the
// overflow map. Every task must run at its own tick: Now() while it runs is
// the end of that tick (the tick is advanced before its tasks run).
TEST_F(TimeManipulatorExecutorTest, TasksOnEveryLevelRunAtTheirTick) {
  Init("{bind_executor: x, dt_us: 1000, wheel_size: [4, 4], init_ratio: 0}");
  executor_.Start();  // paused: the schedule is built against a fixed Now()

  const std::vector<int> offsets_ms = {1, 3, 4, 5, 7, 9, 15, 16, 17, 21, 31, 33, 47, 64, 70, 150};
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::pair<int, std::chrono::nanoseconds>> runs;  // (offset, run time - target)
  const auto base = executor_.Now();
  for (int offset : offsets_ms) {
    const auto target = base + std::chrono::milliseconds(offset);
    executor_.DispatchAt(target, [&, offset, target] {
      std::lock_guard lock(mutex);
      runs.emplace_back(offset, executor_.Now() - target);
      cv.notify_all();
    });
  }
  executor_.SetTimeRatio(4.0);

  std::unique_lock lock(mutex);
  const bool all_ran = cv.wait_for(lock, 10s, [&] { return runs.size() == offsets_ms.size(); });
  lock.unlock();
  executor_.Shutdown();  // no task may touch the locals after the test body
  ASSERT_TRUE(all_ran) << "only " << runs.size() << " of " << offsets_ms.size() << " tasks ran";
  for (size_t i = 0; i < runs.size(); ++i) {
    EXPECT_EQ(runs[i].first, offsets_ms[i]) << "out of order at " << i;
    EXPECT_EQ(runs[i].second, 1ms) << "offset " << runs[i].first << " ms ran at the wrong tick";
  }
}

// Tasks dispatched from tasks running at every tick, so that DispatchAt()
// also runs right before the higher levels and the far map are pulled down
// (the levels below about to wrap): every target must run at its own tick.
TEST(TimeManipulatorExecutorPlacement, TasksDispatchedAtAnyTickRunAtTheirTick) {
  for (const char* wheel : {"[4, 4]", "[2, 3, 2]"}) {
    InlineExecutor inline_executor;
    TimeManipulatorExecutor executor;
    executor.RegisterGetExecutorFunc([&](std::string_view) { return inline_executor.Handle(); });
    executor.Initialize("tm", YAML::Load(std::string("{bind_executor: x, dt_us: 1000, init_ratio: 0, wheel_size: ") +
                                         wheel + "}"));
    executor.Start();

    constexpr int kDispatchers = 30;
    constexpr int kTargets = 30;
    std::mutex mutex;
    std::condition_variable cv;
    int done = 0;
    std::vector<std::chrono::nanoseconds> wrong;  // lateness of tasks not run at their tick
    const auto base = executor.Now();
    for (int d = 0; d < kDispatchers; ++d) {
      executor.DispatchAt(base + std::chrono::milliseconds(d), [&] {
        const auto now = executor.Now();  // runs on the timer thread: time stands still
        for (int k = 0; k < kTargets; ++k) {
          const auto target = now + std::chrono::milliseconds(k);
          executor.DispatchAt(target, [&, target] {
            std::lock_guard lock(mutex);
            if (executor.Now() - target != 1ms) wrong.push_back(executor.Now() - target);
            ++done;
            cv.notify_all();
          });
        }
      });
    }
    executor.SetTimeRatio(1.0);

    std::unique_lock lock(mutex);
    const bool all_ran = cv.wait_for(lock, 10s, [&] { return done == kDispatchers * kTargets; });
    lock.unlock();
    executor.Shutdown();  // no task may touch the locals after this iteration
    ASSERT_TRUE(all_ran) << wheel << ": only " << done << " of " << kDispatchers * kTargets << " tasks ran";
    EXPECT_TRUE(wrong.empty()) << wheel << ": " << wrong.size() << " tasks ran at the wrong tick, e.g. "
                               << std::chrono::duration_cast<std::chrono::microseconds>(wrong.front()).count()
                               << " us after the target";
  }
}

TEST(TimeManipulatorExecutorPlacement, DispatchBeforeStartIsDropped) {
  InlineExecutor inline_executor;
  TimeManipulatorExecutor executor;
  executor.RegisterGetExecutorFunc([&](std::string_view) { return inline_executor.Handle(); });
  executor.Initialize("tm", YAML::Load("{bind_executor: x, dt_us: 1000}"));
  bool ran = false;
  executor.DispatchAt(std::chrono::system_clock::now(), [&ran] { ran = true; });
  executor.Start();
  executor.Shutdown();
  EXPECT_FALSE(ran);
}

}  // namespace
}  // namespace ibmw::extensions::time_manipulator_extension
