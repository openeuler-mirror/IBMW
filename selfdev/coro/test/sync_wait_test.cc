// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ibmw_selfdev/coro/sync_wait.h"

#include <gtest/gtest.h>

#include <coroutine>
#include <latch>

#include "ibmw_selfdev/coro/coro.h"
#include "thread_pool_scheduler.h"

namespace ibmw::selfdev::coro {
namespace {

using test::ThreadPool;

// SW-01
TEST(CoroSyncWait, OtherThreadCompletion) {
  ThreadPool pool(1);
  auto body = [](ThreadPool::Scheduler s) -> Task<int> {
    co_await Schedule(s);
    co_return 9;
  };
  EXPECT_EQ(*SyncWait(body(pool.GetScheduler())), 9);
}

// SW-02
TEST(CoroSyncWait, SynchronousCompletion) {
  EXPECT_TRUE(SyncWait(std::suspend_never{}).has_value());
  auto body = []() -> Task<int> { co_return 3; };
  EXPECT_EQ(*SyncWait(body()), 3);
}

// SD-01
TEST(CoroStartDetached, RunsTask) {
  ThreadPool pool(1);
  std::latch done(1);
  auto body = [](std::latch* l) -> Task<void> {
    l->count_down();
    co_return;
  };
  StartDetached(On(pool.GetScheduler(), body(&done)));
  done.wait();
}

// SD-02: generated-code shape: StartDetached(On(Inline, x) | Then(f)).
TEST(CoroStartDetached, PipeFormFromGeneratedCode) {
  std::latch done(1);
  auto call = []() -> Task<int> { co_return 0; };
  StartDetached(On(InlineScheduler{}, call()) | Then([&](int status) {
                  EXPECT_EQ(status, 0);
                  done.count_down();
                }));
  done.wait();
}

}  // namespace
}  // namespace ibmw::selfdev::coro
