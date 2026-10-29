// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests: executor shutdown/dispatch race, timer executor option
// validation, and rotate-file logger teardown after a failed Initialize().

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

// Must precede the core executor headers: it names ibmw::executor unqualified.
#include "engine/logger/rotate_file_logger_driver.h"

#include "engine/executor/dedicated_executor.h"
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

TEST(ExecutorRobustness, RotateFileLoggerDestructsAfterFailedSyncInitialize) {
  static const ibmw_executor_base_t kDummy{};
  {
    logger::RotateFileLoggerDriver drv;
    drv.RegisterGetExecutorFunc(
        [](std::string_view) { return ibmw::executor::ExecutorHandle(&kDummy); });
    // enable_sync without sync_executor_name fails before the log file is opened.
    EXPECT_ANY_THROW(drv.Initialize(YAML::Load("{enable_sync: true, path: /tmp/ibmw_logger_test}")));
  }  // ~RotateFileLoggerDriver must not fclose(nullptr)
  SUCCEED();
}

}  // namespace
}  // namespace ibmw::runtime::core
