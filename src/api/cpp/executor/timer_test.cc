// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for executor::Timer lifetime and Reset() semantics, and for
// ExecutorHandle::TryDispatch latch accounting.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <thread>
#include <utility>

#include "cpp/executor/timer.h"

namespace ibmw::executor {
namespace {

// Executor that queues timed tasks; tests drain the queue manually.
struct QueueExecutor {
  static ibmw_string_view_t Str(void*) { return ibmw_string_view_t{"queue", 5}; }
  static bool True(void*) { return true; }
  static uint64_t Now(void* ctx) { return static_cast<QueueExecutor*>(ctx)->now_ns; }
  static void Dispatch(void* ctx, ibmw_function_base_t* task) {
    static_cast<QueueExecutor*>(ctx)->tasks.emplace_back(task);
  }
  static void DispatchAt(void* ctx, uint64_t, ibmw_function_base_t* task) { Dispatch(ctx, task); }

  // Runs the currently queued tasks once; returns how many ran.
  size_t RunQueued() {
    size_t n = tasks.size();
    for (size_t i = 0; i < n; ++i) {
      Task t = std::move(tasks.front());
      tasks.pop_front();
      now_ns += 1000000;
      t();
    }
    return n;
  }

  uint64_t now_ns = 1000000000;
  std::deque<Task> tasks;
  ibmw_executor_base_t base{
      .type = &Str,
      .name = &Str,
      .is_thread_safe = &True,
      .is_in_current_executor = &True,
      .supports_timed_dispatch = &True,
      .dispatch = &Dispatch,
      .now = &Now,
      .dispatch_at_ns = &DispatchAt,
      .ctx = this};
};

TEST(ExecutorTimer, DroppingHandleStopsLoopAndReleasesTimer) {
  QueueExecutor exe;
  auto token = std::make_shared<int>(0);
  int calls = 0;
  std::weak_ptr<TimerBase> weak;
  {
    auto timer = CreateTimer(ExecutorHandle(&exe.base), std::chrono::milliseconds(1),
                             std::function<void()>([token, &calls] { ++calls; }));
    weak = timer;
    EXPECT_EQ(exe.RunQueued(), 1u);
    EXPECT_EQ(calls, 1);
  }
  // The pending loop task must not keep the timer (and its captures) alive.
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(token.use_count(), 1);
  exe.RunQueued();
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(exe.tasks.empty());
}

TEST(ExecutorTimer, ResetInsideCallbackKeepsSingleChain) {
  QueueExecutor exe;
  int calls = 0;
  auto timer = CreateTimer(ExecutorHandle(&exe.base), std::chrono::milliseconds(1),
                           std::function<void(TimerBase&)>([&calls](TimerBase& t) {
                             if (++calls == 1) t.Reset();
                           }));
  for (int i = 0; i < 5; ++i) {
    exe.RunQueued();
    EXPECT_LE(exe.tasks.size(), 1u) << "Reset() inside the callback started a second loop";
  }
  timer->Cancel();
  exe.RunQueued();
  EXPECT_TRUE(exe.tasks.empty());
}

// R-N4: Reset() on the old chain's next tick plans the same time point as the
// old chain. On a multi-threaded executor both dispatches can run at once; the
// old chain must not run that tick as well.
TEST(ExecutorTimer, ResetOnOldChainsTickRunsTaskOnce) {
  QueueExecutor exe;
  int calls = 0;
  std::function<void()> run_other;  // the other dispatch, run while a task is in progress
  auto timer = CreateTimer(ExecutorHandle(&exe.base), std::chrono::milliseconds(1),
                           std::function<void()>([&] {
                             ++calls;
                             if (run_other) std::exchange(run_other, nullptr)();
                           }));
  exe.RunQueued();
  ASSERT_EQ(calls, 1);
  timer->Reset();  // the clock has not moved: same planned time as the old chain
  ASSERT_EQ(exe.tasks.size(), 2u);
  Task old_chain = std::move(exe.tasks.front());
  exe.tasks.pop_front();
  Task new_chain = std::move(exe.tasks.front());
  exe.tasks.pop_front();
  run_other = [&new_chain] { new_chain(); };
  old_chain();
  if (run_other) std::exchange(run_other, nullptr)();
  EXPECT_EQ(calls, 2) << "the tick ran on both the old and the new chain";
  EXPECT_EQ(exe.tasks.size(), 1u) << "exactly one chain must continue";
  exe.RunQueued();
  EXPECT_EQ(calls, 3);
  timer->Cancel();
  exe.RunQueued();
  EXPECT_TRUE(exe.tasks.empty());
}

TEST(ExecutorTryDispatch, FailedDispatchRestoresLatchCount) {
  util::DynamicLatch latch;
  ExecutorHandle null_exe;
  EXPECT_ANY_THROW(null_exe.TryDispatch(latch, Task([] {})));
  std::atomic_bool done{false};
  std::thread waiter([&] {
    latch.CloseAndWait();
    done = true;
  });
  for (int i = 0; i < 200 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(done) << "CloseAndWait blocked: TryDispatch leaked a latch count";
  if (!done) {
    latch.CountDown();
  }
  waiter.join();
}

}  // namespace
}  // namespace ibmw::executor
