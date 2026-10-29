// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests: executor shutdown/dispatch race, timer executor option
// validation, and rotate-file logger teardown after a failed Initialize().

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Must precede the core executor headers: it names ibmw::executor unqualified.
#include "engine/logger/rotate_file_logger_driver.h"

#include "engine/executor/dedicated_executor.h"
#include "engine/executor/executor_bus.h"
#include "engine/executor/thread_pool_executor.h"
#include "engine/executor/timer_executor.h"
#include "engine/executor/watchdog_executor.h"

namespace ibmw::runtime::core {
namespace {

auto MakeLogger() { return std::make_shared<ibmw::common::util::LoggerWrapper>(); }

template <class Executor, class InitFn>
void RunDispatchDuringShutdown(InitFn init) {
  for (int round = 0; round < 50; ++round) {
    Executor exe;
    if constexpr (requires { exe.SetLogger(MakeLogger()); }) exe.SetLogger(MakeLogger());
    init(exe);
    exe.Start();

    std::atomic_bool go{false};
    std::atomic_uint32_t ran{0};
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) {
      producers.emplace_back([&] {
        while (!go.load()) {
        }
        for (int i = 0; i < 2000; ++i) exe.Dispatch(ibmw::executor::Task([&ran] { ++ran; }));
      });
    }
    go = true;
    std::this_thread::yield();
    exe.Shutdown();
    for (auto& p : producers) p.join();

    // Every accepted task must have run: nothing may be stranded in the queue.
    EXPECT_EQ(exe.CurrentTaskNum(), 0u) << "round " << round;
  }
}

TEST(ExecutorRobustness, DedicatedDispatchRacingShutdownLeavesNoStrandedTask) {
  RunDispatchDuringShutdown<executor::DedicatedExecutor>(
      [](auto& exe) { exe.Initialize("dedicated_race", YAML::Node()); });
}

TEST(ExecutorRobustness, WatchdogDispatchRacingShutdownLeavesNoStrandedTask) {
  RunDispatchDuringShutdown<executor::WatchdogExecutor>(
      [](auto& exe) { exe.Initialize(YAML::Node()); });
}

void ExpectTimerInitThrows(const char* yaml) {
  executor::TimerExecutor exe;
  exe.SetLogger(MakeLogger());
  exe.RegisterGetExecutorFunc([](std::string_view) { return ibmw::executor::ExecutorHandle(); });
  EXPECT_ANY_THROW(exe.Initialize("timer", YAML::Load(yaml))) << yaml;
}

TEST(ExecutorRobustness, TimerExecutorRejectsDegenerateOptions) {
  ExpectTimerInitThrows("dt_us: 0");
  ExpectTimerInitThrows("wheel_size: []");
  ExpectTimerInitThrows("wheel_size: [100, 0]");
  ExpectTimerInitThrows("wheel_size: [1]");
}

// Wheels of 4 and 4 ticks: targets land on level 0, level 1 and the
// overflow map; every task runs within one tick after its target.
TEST(ExecutorRobustness, TimerExecutorRunsTasksOnEveryLevelAtTheirTick) {
  executor::TimerExecutor exe;
  exe.SetLogger(MakeLogger());
  exe.RegisterGetExecutorFunc([](std::string_view) { return ibmw::executor::ExecutorHandle(); });
  exe.Initialize("timer_levels", YAML::Load("{dt_us: 1000, wheel_size: [4, 4]}"));
  exe.Start();

  const std::vector<int> offsets_ms = {5, 7, 9, 15, 16, 17, 21, 31, 33, 47, 64, 70, 150};
  std::mutex mutex;
  std::vector<std::pair<int, std::chrono::nanoseconds>> runs;  // (offset, run time - target)
  const auto base = exe.Now();
  for (int offset : offsets_ms) {
    const auto target = base + std::chrono::milliseconds(offset);
    exe.DispatchAt(target, ibmw::executor::Task([&, offset, target] {
      std::lock_guard lock(mutex);
      runs.emplace_back(offset, exe.Now() - target);
    }));
  }
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::lock_guard lock(mutex);
    if (runs.size() == offsets_ms.size()) break;
  }
  exe.Shutdown();
  ASSERT_EQ(runs.size(), offsets_ms.size());
  for (size_t i = 0; i < runs.size(); ++i) {
    EXPECT_EQ(runs[i].first, offsets_ms[i]) << "out of order at " << i;
    EXPECT_GE(runs[i].second, std::chrono::nanoseconds(0)) << "offset " << runs[i].first << " ms ran early";
    EXPECT_LE(runs[i].second, std::chrono::milliseconds(2)) << "offset " << runs[i].first << " ms ran late";
  }
}

// Tasks dispatched from tasks running at every tick, so that DispatchAt()
// also runs right before the higher levels and the far map are pulled down:
// every target must run at its own tick (Now() is the end of that tick).
TEST(ExecutorRobustness, TimerExecutorPlacesTasksDispatchedAtAnyTick) {
  for (const char* wheel : {"[4, 4]", "[2, 3, 2]"}) {
    executor::TimerExecutor exe;
    exe.SetLogger(MakeLogger());
    exe.RegisterGetExecutorFunc([](std::string_view) { return ibmw::executor::ExecutorHandle(); });
    exe.Initialize("timer_any_tick", YAML::Load(std::string("{dt_us: 1000, wheel_size: ") + wheel + "}"));
    exe.Start();

    constexpr int kDispatchers = 30;
    constexpr int kTargets = 30;
    std::mutex mutex;
    int done = 0;
    std::vector<std::chrono::nanoseconds> wrong;  // lateness of tasks not run at their tick
    auto dispatch_targets = [&] {
      const auto now = exe.Now();  // runs on the timer thread: time stands still
      for (int k = 0; k < kTargets; ++k) {
        const auto target = now + std::chrono::milliseconds(k);
        exe.DispatchAt(target, ibmw::executor::Task([&, target] {
          std::lock_guard lock(mutex);
          if (exe.Now() - target != std::chrono::milliseconds(1)) wrong.push_back(exe.Now() - target);
          ++done;
        }));
      }
    };
    // The dispatchers are scheduled from the timer thread too, so that none
    // of them is due before it is placed.
    exe.DispatchAt(exe.Now() + std::chrono::milliseconds(20), ibmw::executor::Task([&] {
      const auto now = exe.Now();
      for (int d = 0; d < kDispatchers; ++d)
        exe.DispatchAt(now + std::chrono::milliseconds(d), ibmw::executor::Task(dispatch_targets));
    }));
    for (int i = 0; i < 250; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      std::lock_guard lock(mutex);
      if (done == kDispatchers * kTargets) break;
    }
    exe.Shutdown();
    EXPECT_EQ(done, kDispatchers * kTargets) << wheel;
    EXPECT_TRUE(wrong.empty()) << wheel << ": " << wrong.size() << " tasks ran at the wrong tick";
  }
}

TEST(ExecutorRobustness, RotateFileLoggerDestructsAfterFailedSyncInitialize) {
  static const ibmw_executor_base_t kDummy{};
  const auto log_dir = std::filesystem::temp_directory_path() /
                       ("ibmw_logger_test_" + std::to_string(::getpid()));
  {
    logger::RotateFileLoggerDriver drv;
    drv.RegisterGetExecutorFunc(
        [](std::string_view) { return ibmw::executor::ExecutorHandle(&kDummy); });
    // enable_sync without sync_executor_name fails before the log file is opened.
    EXPECT_ANY_THROW(drv.Initialize(YAML::Load("{enable_sync: true, path: " + log_dir.string() + "}")));
  }  // ~RotateFileLoggerDriver must not fclose(nullptr)
  std::filesystem::remove_all(log_dir);
}

// With 'shutdown_grace_ms: -1' Shutdown() drains everything queued behind a
// slow task instead of stopping the io_context after the grace period.
TEST(ExecutorRobustness, ThreadPoolShutdownGraceIsConfigurable) {
  executor::ThreadPoolExecutor exe;
  exe.SetLogger(MakeLogger());
  YAML::Node options = YAML::Load("{thread_num: 1, shutdown_grace_ms: -1}");
  exe.Initialize("pool_grace", options);
  EXPECT_EQ(options["shutdown_grace_ms"].as<int32_t>(), -1);
  exe.Start();

  std::atomic_uint32_t ran{0};
  exe.Dispatch(ibmw::executor::Task([] { std::this_thread::sleep_for(std::chrono::milliseconds(1200)); }));
  for (int i = 0; i < 10; ++i) exe.Dispatch(ibmw::executor::Task([&ran] { ++ran; }));
  exe.Shutdown();
  EXPECT_EQ(ran.load(), 10u);
}

// CORE-027: a DispatchAt() timer far in the future must not keep Shutdown()
// blocked until it expires; queued immediate tasks still run.
TEST(ExecutorRobustness, ThreadPoolShutdownDoesNotWaitForFarTimers) {
  executor::ThreadPoolExecutor exe;
  exe.SetLogger(MakeLogger());
  exe.Initialize("pool_far_timer", YAML::Load("thread_num: 2"));
  exe.Start();

  std::atomic_bool far_ran{false};
  std::atomic_uint32_t ran{0};
  exe.DispatchAt(exe.Now() + std::chrono::hours(1), ibmw::executor::Task([&] { far_ran = true; }));
  for (int i = 0; i < 100; ++i) exe.Dispatch(ibmw::executor::Task([&ran] { ++ran; }));

  const auto begin = std::chrono::steady_clock::now();
  exe.Shutdown();
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(5));
  EXPECT_EQ(ran.load(), 100u);
  EXPECT_FALSE(far_ran.load());
}

// Without pending timers Shutdown() returns as soon as the queue drains.
TEST(ExecutorRobustness, ThreadPoolShutdownWithoutTimersIsImmediate) {
  executor::ThreadPoolExecutor exe;
  exe.SetLogger(MakeLogger());
  exe.Initialize("pool_no_timer", YAML::Load("thread_num: 2"));
  exe.Start();
  const auto begin = std::chrono::steady_clock::now();
  exe.Shutdown();
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(500));
}

// CORE-029: a strand bound to a thread pool must be shut down and destroyed
// before that pool's io_context (verified by ASan); queued strand work drains.
TEST(ExecutorRobustness, ExecutorBusTearsDownStrandBeforeItsThreadPool) {
  std::atomic_uint32_t ran{0};
  {
    executor::ExecutorBus bus;
    bus.SetLogger(MakeLogger());
    bus.Initialize(YAML::Load(R"(
executors:
  - name: pool
    type: thread_pool
    options: {thread_num: 2}
  - name: serial
    type: strand
    options: {bind_thread_pool_executor_name: pool}
)"));
    bus.Start();
    auto strand = bus.GetExecutor("serial");
    ASSERT_TRUE(strand);
    for (int i = 0; i < 1000; ++i) strand.Dispatch(ibmw::executor::Task([&ran] { ++ran; }));
    bus.Shutdown();
  }
  EXPECT_EQ(ran.load(), 1000u);
}

}  // namespace
}  // namespace ibmw::runtime::core
