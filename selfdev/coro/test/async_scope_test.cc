// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ibmw_selfdev/coro/async_scope.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "ibmw_selfdev/coro/on.h"
#include "ibmw_selfdev/coro/sync_wait.h"
#include "ibmw_selfdev/coro/task.h"
#include "thread_pool_scheduler.h"

namespace ibmw::selfdev::coro {
namespace {

using test::ThreadPool;

Task<void> Increment(std::atomic<int>* n) {
  n->fetch_add(1, std::memory_order_relaxed);
  co_return;
}

// AS-01
TEST(CoroAsyncScope, SpawnStartsEagerly) {
  AsyncScope scope;
  std::atomic<int> n{0};
  EXPECT_TRUE(scope.spawn(Increment(&n)));
  EXPECT_EQ(n.load(), 1);
  SyncWait(scope.complete());
}

// AS-02
TEST(CoroAsyncScope, ManyOnPool) {
  ThreadPool pool(4);
  AsyncScope scope;
  std::atomic<int> n{0};
  for (int i = 0; i < 10000; ++i) ASSERT_TRUE(scope.spawn(On(pool.GetScheduler(), Increment(&n))));
  SyncWait(scope.complete());
  EXPECT_EQ(n.load(), 10000);
  EXPECT_EQ(scope.ActiveCount(), 0u);
}

Task<void> PlainIncrement(int* n) {
  ++*n;
  co_return;
}

// AS-03: single-threaded executor serializes a non-atomic counter.
TEST(CoroAsyncScope, SingleThreadExecutorSerializes) {
  ThreadPool pool(1);
  AsyncScope scope;
  int n = 0;
  for (int i = 0; i < 10000; ++i) scope.spawn(On(pool.GetScheduler(), PlainIncrement(&n)));
  SyncWait(scope.complete());
  EXPECT_EQ(n, 10000);
}

// AS-04
TEST(CoroAsyncScope, SpawnAfterCompleteIsRejected) {
  AsyncScope scope;
  SyncWait(scope.complete());
  std::atomic<int> n{0};
  auto owner = std::make_shared<int>(0);
  std::weak_ptr<int> weak = owner;
  auto body = [](std::shared_ptr<int>, std::atomic<int>* c) -> Task<void> {
    c->fetch_add(1);
    co_return;
  };
  EXPECT_FALSE(scope.spawn(body(std::move(owner), &n)));
  EXPECT_EQ(n.load(), 0);
  EXPECT_TRUE(weak.expired());
  EXPECT_TRUE(scope.Closed());
}

// AS-05
TEST(CoroAsyncScope, EmptyCompleteIsReady) {
  AsyncScope scope;
  auto awaiter = scope.complete();
  EXPECT_TRUE(awaiter.await_ready());
}

// AS-06
TEST(CoroAsyncScope, MultipleWaitersAllResume) {
  ThreadPool pool(2);
  auto scope = std::make_unique<AsyncScope>();
  std::atomic<bool> release{false};
  auto gate = [](std::atomic<bool>* r, ThreadPool::Scheduler s) -> Task<void> {
    while (!r->load()) co_await Schedule(s);
  };
  ASSERT_TRUE(scope->spawn(On(pool.GetScheduler(), gate(&release, pool.GetScheduler()))));

  std::atomic<int> woken{0};
  std::vector<std::thread> waiters;
  for (int i = 0; i < 3; ++i) {
    waiters.emplace_back([&] {
      SyncWait(scope->complete());
      woken.fetch_add(1);
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(woken.load(), 0);
  release = true;
  for (auto& t : waiters) t.join();
  EXPECT_EQ(woken.load(), 3);
  SyncWait(scope->complete());  // again, already empty
}

// AS-07: like executor_co_module, wait for a scope from inside a coroutine.
TEST(CoroAsyncScope, AwaitCompleteInsideCoroutine) {
  ThreadPool pool(2);
  std::atomic<int> n{0};
  auto body = [](ThreadPool* p, std::atomic<int>* c) -> Task<int> {
    AsyncScope scope;
    for (int i = 0; i < 1000; ++i) scope.spawn(On(p->GetScheduler(), Increment(c)));
    co_await On(p->GetScheduler(), scope.complete());
    co_return c->load();
  };
  EXPECT_EQ(*SyncWait(body(&pool, &n)), 1000);
}

// AS-08: a scope on a coroutine frame is destroyed right after its waiter
// resumes on the last finisher's thread (ASAN checks for use-after-free).
TEST(CoroAsyncScope, LocalScopeDestroyedAfterWake) {
  ThreadPool pool(4);
  auto body = [](ThreadPool* p) -> Task<void> {
    auto scope = std::make_unique<AsyncScope>();
    std::atomic<int> n{0};
    for (int i = 0; i < 64; ++i) scope->spawn(On(p->GetScheduler(), Increment(&n)));
    co_await scope->complete();
    scope.reset();
  };
  for (int i = 0; i < 200; ++i) SyncWait(body(&pool));
}

// AS-09
TEST(CoroAsyncScope, ActiveCountAndClosed) {
  ThreadPool pool(1);
  AsyncScope scope;
  EXPECT_EQ(scope.ActiveCount(), 0u);
  EXPECT_FALSE(scope.Closed());
  std::atomic<bool> release{false};
  auto gate = [](std::atomic<bool>* r, ThreadPool::Scheduler s) -> Task<void> {
    while (!r->load()) co_await Schedule(s);
  };
  scope.spawn(On(pool.GetScheduler(), gate(&release, pool.GetScheduler())));
  scope.spawn(On(pool.GetScheduler(), gate(&release, pool.GetScheduler())));
  EXPECT_EQ(scope.ActiveCount(), 2u);
  release = true;
  SyncWait(scope.complete());
  EXPECT_EQ(scope.ActiveCount(), 0u);
  EXPECT_TRUE(scope.Closed());
}

// AS-10
TEST(CoroAsyncScope, ConcurrentSpawnAndComplete) {
  ThreadPool pool(4);
  // 500 rounds x 4 spawners x 50 = 100k spawns.
  for (int round = 0; round < 500; ++round) {
    AsyncScope scope;
    std::atomic<int> accepted{0};
    std::atomic<int> ran{0};
    std::vector<std::thread> spawners;
    for (int t = 0; t < 4; ++t) {
      spawners.emplace_back([&] {
        for (int i = 0; i < 50; ++i) {
          if (scope.spawn(On(pool.GetScheduler(), Increment(&ran)))) accepted.fetch_add(1);
        }
      });
    }
    std::thread completer([&] { SyncWait(scope.complete()); });
    for (auto& t : spawners) t.join();
    completer.join();
    SyncWait(scope.complete());
    EXPECT_EQ(ran.load(), accepted.load());
  }
}

// AS-11
TEST(CoroAsyncScope, CleanupBehavesLikeComplete) {
  ThreadPool pool(2);
  AsyncScope scope;
  std::atomic<int> n{0};
  for (int i = 0; i < 100; ++i) scope.spawn(On(pool.GetScheduler(), Increment(&n)));
  SyncWait(scope.cleanup());
  EXPECT_EQ(n.load(), 100);
  EXPECT_TRUE(scope.Closed());
  EXPECT_FALSE(scope.spawn(Increment(&n)));
}

// AS-12: copying the awaitable throws inside spawn(); the reservation is
// rolled back and the scope can still complete.
struct ThrowOnCopy {
  ThrowOnCopy() = default;
  ThrowOnCopy(const ThrowOnCopy&) { throw std::runtime_error("copy"); }
  ThrowOnCopy(ThrowOnCopy&&) noexcept = default;
  bool await_ready() const noexcept { return true; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  void await_resume() const noexcept {}
};

TEST(CoroAsyncScope, SpawnRollsBackWhenCopyThrows) {
  AsyncScope scope;
  ThrowOnCopy a;
  EXPECT_THROW(scope.spawn(a), std::runtime_error);
  EXPECT_EQ(scope.ActiveCount(), 0u);
  EXPECT_FALSE(scope.Closed());
  EXPECT_TRUE(scope.spawn(ThrowOnCopy{}));
  SyncWait(scope.complete());
  EXPECT_EQ(scope.ActiveCount(), 0u);
}

// AS-13: an exception escaping spawned work is fatal (as with
// unifex::async_scope::spawn), not silently dropped.
TEST(CoroAsyncScopeDeathTest, EscapingExceptionTerminates) {
  EXPECT_DEATH(
      {
        AsyncScope scope;
        auto boom = []() -> Task<void> {
          throw std::runtime_error("boom");
          co_return;
        };
        scope.spawn(boom());
      },
      "");
}

}  // namespace
}  // namespace ibmw::selfdev::coro
