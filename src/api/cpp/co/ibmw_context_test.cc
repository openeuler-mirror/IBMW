// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression test: IbmwExecutor operation states must free the heap receiver
// they create in start() once the dispatched task has run.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <vector>

#include "cpp/co/ibmw_context.h"

#ifndef IBMW_EXECUTOR_USE_STDEXEC

namespace ibmw::co {
namespace {

// Minimal inline executor: runs dispatched tasks immediately on the caller.
struct InlineExecutorFixture {
  static ibmw_string_view_t Str(void*) { return ibmw_string_view_t{"inline", 6}; }
  static bool True(void*) { return true; }
  static uint64_t Now(void*) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
  }
  static void Dispatch(void*, ibmw_function_base_t* task) {
    executor::Task t(task);
    t();
  }
  static void DispatchAt(void* ctx, uint64_t, ibmw_function_base_t* task) { Dispatch(ctx, task); }

  ibmw_executor_base_t base{
      .type = &Str,
      .name = &Str,
      .is_thread_safe = &True,
      .is_in_current_executor = &True,
      .supports_timed_dispatch = &True,
      .dispatch = &Dispatch,
      .now = &Now,
      .dispatch_at_ns = &DispatchAt,
      .ctx = nullptr};
};

struct ReceiverCounters {
  int live = 0;
  int values = 0;
};

// Receiver that tracks how many instances are alive.
struct CountingReceiver {
  explicit CountingReceiver(ReceiverCounters* c) : counters(c) { ++counters->live; }
  CountingReceiver(CountingReceiver&& o) noexcept : counters(o.counters) { ++counters->live; }
  CountingReceiver(const CountingReceiver&) = delete;
  ~CountingReceiver() { --counters->live; }

  void set_value() && { ++counters->values; }
  void set_error(std::exception_ptr) && noexcept {}
  void set_done() && noexcept {}

  ReceiverCounters* counters;
};

template <typename Sender>
void RunOnce(Sender&& sender, ReceiverCounters& counters) {
  auto op = std::forward<Sender>(sender).connect(CountingReceiver(&counters));
  op.start();
}

TEST(IbmwExecutorReceiverLifetime, ScheduleDoesNotLeakReceiver) {
  InlineExecutorFixture fx;
  IbmwExecutor exe{executor::ExecutorHandle(&fx.base)};
  ReceiverCounters counters;
  for (int i = 0; i < 16; ++i) RunOnce(exe.schedule(), counters);
  EXPECT_EQ(counters.values, 16);
  EXPECT_EQ(counters.live, 0);
}

TEST(IbmwExecutorReceiverLifetime, ScheduleAfterDoesNotLeakReceiver) {
  InlineExecutorFixture fx;
  IbmwExecutor exe{executor::ExecutorHandle(&fx.base)};
  ReceiverCounters counters;
  for (int i = 0; i < 16; ++i) RunOnce(exe.schedule_after(std::chrono::nanoseconds(1)), counters);
  EXPECT_EQ(counters.values, 16);
  EXPECT_EQ(counters.live, 0);
}

TEST(IbmwExecutorReceiverLifetime, ScheduleAtDoesNotLeakReceiver) {
  InlineExecutorFixture fx;
  IbmwExecutor exe{executor::ExecutorHandle(&fx.base)};
  ReceiverCounters counters;
  for (int i = 0; i < 16; ++i) RunOnce(exe.schedule_at(std::chrono::system_clock::now()), counters);
  EXPECT_EQ(counters.values, 16);
  EXPECT_EQ(counters.live, 0);
}

}  // namespace
}  // namespace ibmw::co

#endif  // IBMW_EXECUTOR_USE_STDEXEC
