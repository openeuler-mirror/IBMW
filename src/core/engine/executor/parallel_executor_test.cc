// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// ParallelExecutor integration tests. They run against whichever queue
// backend is configured (selfdev BoundedBlockingQueue or tbb).

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine/executor/parallel_executor.h"

namespace ibmw::runtime::core::executor {
namespace {

using namespace std::chrono_literals;

// Waits until pred() holds or the deadline passes; returns pred().
template <typename Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  return pred();
}

TEST(ParallelExecutorTest, DispatchAfterShutdownDoesNotRunTask) {
  ParallelExecutor exe;
  exe.Initialize("parallel_after_shutdown", YAML::Load("{thread_num: 2}"));
  exe.Start();

  constexpr int kTasks = 200;
  std::atomic_int executed = 0;
  for (int ii = 0; ii < kTasks; ++ii) exe.Dispatch([&executed]() { ++executed; });
  ASSERT_TRUE(WaitFor([&]() { return executed.load() == kTasks; }, 5000ms));
  ASSERT_TRUE(WaitFor([&]() { return exe.CurrentTaskNum() == 0; }, 5000ms));

  exe.Shutdown();
  EXPECT_EQ(exe.GetState(), ParallelExecutor::State::kShutdown);

  // The rejected task is dropped (its captures released), never run.
  auto token = std::make_shared<int>(7);
  std::atomic_bool ran = false;
  exe.Dispatch([token, &ran]() { ran = true; });
  EXPECT_FALSE(ran.load());
  EXPECT_EQ(token.use_count(), 1);
  EXPECT_EQ(exe.CurrentTaskNum(), 0u);

  // Shutdown is idempotent.
  exe.Shutdown();
  EXPECT_EQ(exe.CurrentTaskNum(), 0u);
  EXPECT_EQ(executed.load(), kTasks);
}

TEST(ParallelExecutorTest, DispatchRacingShutdownNeitherCrashesNorHangs) {
  for (int round = 0; round < 5; ++round) {
    ParallelExecutor exe;
    exe.Initialize("parallel_race_shutdown", YAML::Load("{thread_num: 4, queue_threshold: 1000000}"));
    exe.Start();

    constexpr int kProducers = 4;
    constexpr int kExtraAfterShutdown = 5;
    std::atomic_int64_t attempted = 0;
    std::atomic_int64_t executed = 0;
    std::vector<std::thread> producers;
    for (int ii = 0; ii < kProducers; ++ii) {
      producers.emplace_back([&]() {
        while (exe.GetState() != ParallelExecutor::State::kShutdown) {
          ++attempted;
          exe.Dispatch([&executed]() { ++executed; });
        }
        // A few dispatches that are certainly after Shutdown started.
        for (int jj = 0; jj < kExtraAfterShutdown; ++jj) {
          ++attempted;
          exe.Dispatch([&executed]() { ++executed; });
        }
      });
    }

    std::this_thread::sleep_for(20ms);
    exe.Shutdown();
    for (auto& t : producers) t.join();

    const int64_t attempted_n = attempted.load();
    const int64_t executed_n = executed.load();
    const int64_t pending = static_cast<int64_t>(exe.CurrentTaskNum());
    EXPECT_GT(executed_n, 0);
    EXPECT_LE(executed_n, attempted_n);
    // Tasks that slipped in during Shutdown may stay counted (tbb keeps them
    // queued), but the counter must never exceed what is really unexecuted.
    EXPECT_LE(pending, attempted_n - executed_n) << "round " << round;

    // Nothing runs once the workers have been joined.
    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(executed.load(), executed_n);
  }
}

// Regression test for the queue counter wrap: the counter used to be
// incremented after Emplace, so a worker could pop and decrement first,
// wrapping the uint32 counter to ~2^32 and making the threshold check reject
// tasks while the queue was nearly empty. Producers alternate bursts with
// yields so idle workers often grab a task right after it is inserted.
TEST(ParallelExecutorTest, ConcurrentDispatchCounterNeverWraps) {
  constexpr int kRounds = 3;
  constexpr int kProducers = 8;
  constexpr int kPerProducer = 20000;
  constexpr uint64_t kTotal = static_cast<uint64_t>(kProducers) * kPerProducer;

  for (int round = 0; round < kRounds; ++round) {
    ParallelExecutor exe;
    exe.Initialize("parallel_counter",
                   YAML::Load("{thread_num: 4, queue_threshold: " + std::to_string(2 * kTotal) +
                              ", threshold_alarm_interval_ms: 1000}"));
    exe.Start();

    std::atomic_uint64_t executed = 0;
    std::atomic_bool producing = true;
    std::atomic_uint64_t max_seen = 0;

    std::thread sampler([&]() {
      while (producing.load()) {
        const uint64_t cur = exe.CurrentTaskNum();
        uint64_t prev = max_seen.load();
        while (cur > prev && !max_seen.compare_exchange_weak(prev, cur)) {
        }
      }
    });

    std::vector<std::thread> producers;
    for (int ii = 0; ii < kProducers; ++ii) {
      producers.emplace_back([&, ii]() {
        for (int jj = 0; jj < kPerProducer; ++jj) {
          exe.Dispatch([&executed]() { ++executed; });
          if ((jj + ii) % 8 == 0) std::this_thread::yield();
        }
      });
    }
    for (auto& t : producers) t.join();

    // Every task was accepted (the queue never reached the threshold) and runs.
    EXPECT_TRUE(WaitFor([&]() { return executed.load() == kTotal; }, 20000ms))
        << "round " << round << ": executed " << executed.load() << " of " << kTotal;
    producing = false;
    sampler.join();

    EXPECT_LE(max_seen.load(), kTotal) << "round " << round;
    EXPECT_LT(max_seen.load(), uint64_t{1} << 31) << "round " << round;
    EXPECT_TRUE(WaitFor([&]() { return exe.CurrentTaskNum() == 0; }, 5000ms))
        << "round " << round << ": CurrentTaskNum " << exe.CurrentTaskNum();

    exe.Shutdown();
    if (HasFailure()) break;
  }
}

TEST(ParallelExecutorTest, ThresholdRejectsOnlyAtLimit) {
  ParallelExecutor exe;
  exe.Initialize("parallel_threshold",
                 YAML::Load("{thread_num: 1, queue_threshold: 10, threshold_alarm_interval_ms: 1000}"));
  exe.Start();

  // Block the only worker so queued tasks accumulate.
  std::atomic_bool release = false;
  std::atomic_bool blocker_running = false;
  exe.Dispatch([&]() {
    blocker_running = true;
    while (!release.load()) std::this_thread::sleep_for(1ms);
  });
  ASSERT_TRUE(WaitFor([&]() { return blocker_running.load(); }, 5000ms));

  std::atomic_int executed = 0;
  for (int ii = 0; ii < 20; ++ii) exe.Dispatch([&executed]() { ++executed; });
  // The blocker is still counted, so 9 more fit below the threshold of 10.
  EXPECT_EQ(exe.CurrentTaskNum(), 10u);

  release = true;
  EXPECT_TRUE(WaitFor([&]() { return exe.CurrentTaskNum() == 0; }, 5000ms));
  EXPECT_EQ(executed.load(), 9);

  exe.Shutdown();
}

}  // namespace
}  // namespace ibmw::runtime::core::executor
