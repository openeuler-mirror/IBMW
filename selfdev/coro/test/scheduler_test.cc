// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ibmw_selfdev/coro/scheduler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include "ibmw_selfdev/coro/concepts.h"
#include "ibmw_selfdev/coro/on.h"
#include "ibmw_selfdev/coro/sync_wait.h"
#include "ibmw_selfdev/coro/task.h"
#include "ibmw_selfdev/coro/then.h"
#include "thread_pool_scheduler.h"

namespace ibmw::selfdev::coro {
namespace {

using test::ThreadPool;
using namespace std::chrono_literals;

// SC-05
static_assert(Scheduler<ThreadPool::Scheduler>);
static_assert(TimedScheduler<ThreadPool::Scheduler>);
static_assert(Scheduler<InlineScheduler>);
static_assert(!TimedScheduler<InlineScheduler>);
static_assert(!Scheduler<int>);
static_assert(Awaitable<Task<int>>);
static_assert(!Awaitable<int>);
static_assert(std::is_same_v<AwaitValue<Task<int>>, int>);
static_assert(std::is_void_v<AwaitValue<Task<void>>>);

// SC-01
TEST(CoroSchedule, SwitchesToPoolThread) {
  ThreadPool pool(2);
  auto body = [](ThreadPool* p) -> Task<bool> {
    co_await Schedule(p->GetScheduler());
    co_return p->IsWorkerThread();
  };
  EXPECT_TRUE(*SyncWait(body(&pool)));
}

// SC-02
TEST(CoroSchedule, InlineSchedulerStaysOnThread) {
  auto caller = detail::CurrentThreadId();
  auto body = []() -> Task<std::thread::id> {
    co_await Schedule(InlineScheduler{});
    co_return detail::CurrentThreadId();
  };
  EXPECT_EQ(*SyncWait(body()), caller);
}

// SC-03
TEST(CoroSchedule, ScheduleAfterDelays) {
  ThreadPool pool(1);
  auto start = std::chrono::steady_clock::now();
  auto body = [](ThreadPool::Scheduler s) -> Task<void> { co_await ScheduleAfter(s, 50ms); };
  SyncWait(body(pool.GetScheduler()));
  EXPECT_GE(std::chrono::steady_clock::now() - start, 50ms);
}

// SC-04
TEST(CoroSchedule, ScheduleAtDelays) {
  ThreadPool pool(1);
  auto start = std::chrono::steady_clock::now();
  auto body = [](ThreadPool::Scheduler s) -> Task<void> {
    co_await ScheduleAt(s, std::chrono::system_clock::now() + 50ms);
  };
  SyncWait(body(pool.GetScheduler()));
  EXPECT_GE(std::chrono::steady_clock::now() - start, 50ms);
}

Task<int> WorkerCheck(ThreadPool* p) { co_return p->IsWorkerThread() ? 1 : 0; }

// ON-01
TEST(CoroOn, RunsOnScheduler) {
  ThreadPool pool(2);
  EXPECT_EQ(*SyncWait(On(pool.GetScheduler(), WorkerCheck(&pool))), 1);
}

// ON-02
TEST(CoroOn, InlineRunsOnCaller) {
  auto caller = detail::CurrentThreadId();
  auto body = []() -> Task<std::thread::id> { co_return detail::CurrentThreadId(); };
  EXPECT_EQ(*SyncWait(On(InlineScheduler{}, body())), caller);
}

// ON-03
TEST(CoroOn, PropagatesException) {
  ThreadPool pool(1);
  auto body = []() -> Task<int> {
    throw std::logic_error("on");
    co_return 0;
  };
  EXPECT_THROW(SyncWait(On(pool.GetScheduler(), body())), std::logic_error);
}

// ON-04: On() is lazy and still converts to the Task it used to return.
TEST(CoroOn, LazyAndConvertibleToTask) {
  ThreadPool pool(1);
  bool started = false;
  auto body = [](bool* s) -> Task<int> {
    *s = true;
    co_return 7;
  };
  auto on = On(pool.GetScheduler(), body(&started));
  EXPECT_FALSE(started);
  Task<int> t = std::move(on);
  EXPECT_FALSE(started);
  EXPECT_EQ(*SyncWait(std::move(t)), 7);
  EXPECT_TRUE(started);
}

Task<int> Twenty_one() { co_return 21; }

// TH-01
TEST(CoroThen, TransformsValue) {
  EXPECT_EQ(*SyncWait(Then(Twenty_one(), [](int x) { return x * 2; })), 42);
}

// TH-02
TEST(CoroThen, VoidInput) {
  bool called = false;
  auto body = []() -> Task<void> { co_return; };
  SyncWait(Then(body(), [&] { called = true; }));
  EXPECT_TRUE(called);
}

// TH-03: pipe form emitted by the IBMW code generators.
TEST(CoroThen, PipeForm) {
  ThreadPool pool(1);
  bool on_worker = false;
  auto chained = On(pool.GetScheduler(), Twenty_one()) | Then([&](int x) {
                   on_worker = pool.IsWorkerThread();
                   return x + 21;
                 });
  EXPECT_EQ(*SyncWait(std::move(chained)), 42);
  EXPECT_TRUE(on_worker);

  int seen = 0;
  SyncWait(On(InlineScheduler{}, Twenty_one()) | Then([&](int x) { seen = x; }));
  EXPECT_EQ(seen, 21);
}

// TH-04: Then propagates an exception from f.
TEST(CoroThen, PropagatesExceptionFromFunction) {
  auto t = Then(Twenty_one(), [](int) -> int { throw std::out_of_range("f"); });
  EXPECT_THROW(SyncWait(std::move(t)), std::out_of_range);
}

}  // namespace
}  // namespace ibmw::selfdev::coro
