// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for the time manipulator executor:
// EXT-016: invalid dt / wheel sizes are rejected at initialization.
// EXT-017: virtual time is in the system clock domain, time points before
// the start are due at once, and tasks on every wheel level (and beyond)
// run at their tick.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
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
// overflow map. Every task must run at its own tick (Now() while it runs).
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
  ASSERT_TRUE(cv.wait_for(lock, 10s, [&] { return runs.size() == offsets_ms.size(); }))
      << "only " << runs.size() << " of " << offsets_ms.size() << " tasks ran";
  for (size_t i = 0; i < runs.size(); ++i) {
    EXPECT_EQ(runs[i].first, offsets_ms[i]) << "out of order at " << i;
    // Run at the tick containing the target: at most one dt early, never late.
    EXPECT_LE(runs[i].second, 0ns) << "offset " << runs[i].first << " ms ran late";
    EXPECT_GT(runs[i].second, -1ms) << "offset " << runs[i].first << " ms ran early";
  }
}

}  // namespace
}  // namespace ibmw::extensions::time_manipulator_extension
