// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Awaiter rules (see README.md, "Rules for awaiters") and the review fixes
// around them:
//  - GCC 11/12 destroys the members of an aggregate-initialized co_await
//    operand twice (`co_await Aw{x}`, `co_await Aw(x)` with C++20
//    parenthesized aggregate init). Awaiters get a user-declared constructor,
//    or are named locals, or are returned by a function / operator co_await.
//    These tests count constructions and destructions of a member of such
//    awaiters through Task, and of values flowing through every library
//    primitive, to show that the library itself destroys nothing twice.
//  - Then passes f exactly what co_await yields; lvalue Tasks are rejected
//    by a constraint; const rvalue OnAwaitables are rejected.
//  - ~AsyncScope with active work terminates in release builds too.
//  - StartDetached reports work dropped because the scope is closed.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <type_traits>
#include <utility>

#include "ibmw_selfdev/coro/async_scope.h"
#include "ibmw_selfdev/coro/export.h"
#include "ibmw_selfdev/coro/on.h"
#include "ibmw_selfdev/coro/scheduler.h"
#include "ibmw_selfdev/coro/start_detached.h"
#include "ibmw_selfdev/coro/sync_wait.h"
#include "ibmw_selfdev/coro/task.h"
#include "ibmw_selfdev/coro/then.h"
#include "thread_pool_scheduler.h"

namespace ibmw::selfdev::coro {
namespace {

// Counts constructions / destructions; a destructor running on an object
// that is not alive (already destroyed, or never constructed) is recorded.
struct Counts {
  std::atomic<int> constructed{0};
  std::atomic<int> destroyed{0};
  std::atomic<int> bad_destroy{0};
  int Live() const { return constructed.load() - destroyed.load(); }
};

class Counter {
 public:
  static constexpr unsigned kAlive = 0xA11CEu;

  explicit Counter(Counts* c, int v = 0) noexcept : c_(c), value_(v) { c_->constructed.fetch_add(1); }
  Counter(const Counter& o) noexcept : c_(o.c_), value_(o.value_) { c_->constructed.fetch_add(1); }
  Counter(Counter&& o) noexcept : c_(o.c_), value_(o.value_) { c_->constructed.fetch_add(1); }
  Counter& operator=(const Counter&) = delete;
  Counter& operator=(Counter&&) = delete;
  ~Counter() {
    if (alive_ != kAlive) {
      c_->bad_destroy.fetch_add(1);
      return;
    }
    alive_ = 0;
    c_->destroyed.fetch_add(1);
  }

  int value() const noexcept { return value_; }

 private:
  Counts* c_;
  int value_;
  volatile unsigned alive_ = kAlive;
};

// Non-aggregate awaiter (user-declared constructor) holding a non-trivially
// destructible member. Completes synchronously (await_suspend returns false)
// or resumes the coroutine from a new thread.
class CountingAwaiter {
 public:
  CountingAwaiter(Counts* c, int v, bool async) noexcept : member_(c, v), async_(async) {}

  bool await_ready() const noexcept { return false; }

  // noinline + nothing touched after publishing h (Clang HALO rule).
  IBMW_SELFDEV_CORO_NOINLINE bool await_suspend(std::coroutine_handle<> h) const {
    if (!async_) return false;
    std::thread([h] { h.resume(); }).detach();
    return true;
  }

  int await_resume() const noexcept { return member_.value(); }

 private:
  Counter member_;
  bool async_;
};

void ExpectBalanced(const Counts& c) {
  EXPECT_EQ(c.bad_destroy.load(), 0);
  EXPECT_EQ(c.Live(), 0);
  EXPECT_GT(c.constructed.load(), 0);
}

// CO-18: `co_await CountingAwaiter(...)` (a temporary of a non-aggregate
// awaiter) inside Task: every member constructed is destroyed exactly once.
TEST(CoroAwaiterRules, NonAggregateTemporaryDestroyedOnce) {
  for (bool async : {false, true}) {
    Counts c;
    auto body = [&]() -> Task<int> {
      int sum = 0;
      for (int i = 0; i < (async ? 200 : 10000); ++i) sum += co_await CountingAwaiter(&c, 1, async);
      co_return sum;
    };
    EXPECT_EQ(*SyncWait(body()), async ? 200 : 10000);
    ExpectBalanced(c);
    EXPECT_EQ(c.constructed.load(), async ? 200 : 10000) << "one member per awaiter, no copies";
  }
}

// CO-19: named awaiter (`Aw a(...); co_await a;`), the other safe form.
TEST(CoroAwaiterRules, NamedAwaiterDestroyedOnce) {
  for (bool async : {false, true}) {
    Counts c;
    auto body = [&]() -> Task<int> {
      int sum = 0;
      for (int i = 0; i < 100; ++i) {
        CountingAwaiter aw(&c, 2, async);
        sum += co_await aw;
      }
      co_return sum;
    };
    EXPECT_EQ(*SyncWait(body()), 200);
    ExpectBalanced(c);
    EXPECT_EQ(c.constructed.load(), 100);
  }
}

// The library's own awaiters are never aggregates, final awaiters included.
// (Awaiters that a library function returns by value, e.g.
// InlineScheduler::schedule()'s std::suspend_never, are safe whatever their
// type: the shape GCC gets wrong is only a braced / parenthesized aggregate
// temporary written in the co_await operand itself.)
static_assert(!std::is_aggregate_v<std::remove_cvref_t<detail::AwaiterOf<Task<int>>>>);
static_assert(!std::is_aggregate_v<std::remove_cvref_t<detail::AwaiterOf<Task<void>>>>);
static_assert(!std::is_aggregate_v<detail::TaskPromiseBase::FinalAwaiter>);
static_assert(!std::is_aggregate_v<detail::SyncWaitDriver<int>::promise_type::FinalAwaiter>);
static_assert(!std::is_aggregate_v<AsyncScope::CompleteAwaiter>);
static_assert(!std::is_aggregate_v<detail::OnAwaitable<InlineScheduler, Task<int>>>);
static_assert(!std::is_aggregate_v<test::ThreadPool::Scheduler::Awaiter>);

Task<Counter> MakeCounter(Counts* c, int v) { co_return Counter(c, v); }

Task<int> TakeCounter(Counter arg) { co_return arg.value(); }

Task<void> AddCounter(Counter arg, std::atomic<int>* sum) {
  sum->fetch_add(arg.value());
  co_return;
}

// CO-20: values (Task results, coroutine arguments, captured state) flowing
// through Task, Then, On, SyncWait, AsyncScope and StartDetached are each
// destroyed exactly once, synchronously and across threads.
TEST(CoroAwaiterRules, LibraryDestroysValuesOnce) {
  Counts c;
  {
    test::ThreadPool pool(2);
    auto sched = pool.GetScheduler();

    // Task result and argument, nested, synchronous completion.
    auto body = [&]() -> Task<int> {
      int sum = 0;
      for (int i = 0; i < 100; ++i) {
        Counter v = co_await MakeCounter(&c, 1);
        sum += co_await TakeCounter(std::move(v));
        sum += co_await TakeCounter(Counter(&c, 1));
      }
      co_return sum;
    };
    EXPECT_EQ(*SyncWait(body()), 200);

    // SyncWait of a Task<Counter>.
    EXPECT_EQ(SyncWait(MakeCounter(&c, 7))->value(), 7);

    // Then: f takes the awaited Counter by value, const& and &&; f returns a
    // Counter; f captures a Counter.
    EXPECT_EQ(*SyncWait(Then(MakeCounter(&c, 3), [](Counter v) { return v.value(); })), 3);
    EXPECT_EQ(*SyncWait(MakeCounter(&c, 4) | Then([](const Counter& v) { return v.value(); })), 4);
    EXPECT_EQ(*SyncWait(MakeCounter(&c, 5) | Then([](Counter&& v) { return v.value(); })), 5);
    EXPECT_EQ(SyncWait(TakeCounter(Counter(&c, 6)) | Then([&c, cap = Counter(&c, 1)](int v) {
                         return Counter(&c, v + cap.value());
                       }))->value(),
              7);

    // On / On | Then across threads.
    EXPECT_EQ(SyncWait(On(sched, MakeCounter(&c, 8)))->value(), 8);
    EXPECT_EQ(*SyncWait(On(sched, MakeCounter(&c, 9)) | Then([](Counter v) { return v.value(); })), 9);
    Task<Counter> converted = On(sched, MakeCounter(&c, 10));
    EXPECT_EQ(SyncWait(std::move(converted))->value(), 10);

    // A Counter awaited after hopping to the pool (completes on a worker).
    auto hop = [&]() -> Task<int> {
      int sum = 0;
      for (int i = 0; i < 50; ++i) {
        co_await Schedule(sched);
        Counter v = co_await MakeCounter(&c, 1);
        sum += co_await CountingAwaiter(&c, v.value(), (i % 2) == 0);
      }
      co_return sum;
    };
    EXPECT_EQ(*SyncWait(hop()), 50);

    // AsyncScope: spawned Tasks own a Counter argument; one is never awaited
    // (dropped by a closed scope), as is a Counter-capturing awaitable.
    std::atomic<int> sum{0};
    AsyncScope scope;
    for (int i = 0; i < 20; ++i) EXPECT_TRUE(scope.spawn(On(sched, AddCounter(Counter(&c, 1), &sum))));
    SyncWait(scope.complete());
    EXPECT_FALSE(scope.spawn(AddCounter(Counter(&c, 100), &sum)));
    EXPECT_EQ(sum.load(), 20);

    // StartDetached on the process-wide scope.
    std::atomic<int> detached{0};
    for (int i = 0; i < 20; ++i) EXPECT_TRUE(StartDetached(On(sched, AddCounter(Counter(&c, 1), &detached))));
    for (int i = 0; i < 5000 && detached.load() != 20; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(detached.load(), 20);
    // The last AddCounter frame (and its Counter) is destroyed after the
    // increment; wait for the scope to drop its count.
    for (int i = 0; i < 5000 && c.Live() != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ExpectBalanced(c);
}

// An awaitable whose await_resume yields an lvalue reference.
class RefAwaitable {
 public:
  explicit RefAwaitable(int* p) noexcept : p_(p) {}
  bool await_ready() const noexcept { return true; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  int& await_resume() const noexcept { return *p_; }

 private:
  int* p_;
};

static_assert(std::is_same_v<AwaitResult<RefAwaitable>, int&>);
static_assert(std::is_same_v<AwaitResult<Task<int>>, int>);

// CO-21: Then passes f exactly what co_await yields (not a decayed copy):
// f(int&) binds to an awaitable yielding int&, f(int&&) to Task<int>.
TEST(CoroThenForwarding, PassesUndecayedAwaitResult) {
  int x = 1;
  auto inc = [](int& r) { return ++r; };
  EXPECT_EQ(*SyncWait(Then(RefAwaitable(&x), inc)), 2);
  EXPECT_EQ(*SyncWait(RefAwaitable(&x) | Then(inc)), 3);
  EXPECT_EQ(x, 3) << "f received a reference to x, not a copy";
  auto by_rvalue = [](int&& v) { return v * 2; };
  EXPECT_EQ(*SyncWait(Then([]() -> Task<int> { co_return 21; }(), by_rvalue)), 42);
  EXPECT_EQ(*SyncWait([]() -> Task<int> { co_return 4; }() | Then(by_rvalue)), 8);
  // void f with a reference argument.
  SyncWait(RefAwaitable(&x) | Then([](int& r) { r = 10; }));
  EXPECT_EQ(x, 10);
}

struct AnyInt {
  int operator()(int v) const { return v; }
};

template <class A>
concept CanPipeThen = requires(A&& a) { static_cast<A&&>(a) | Then(AnyInt{}); };

template <class A>
concept CanCallThen = requires(A&& a) { Then(static_cast<A&&>(a), AnyInt{}); };

template <class A>
concept CanCoAwaitMember = requires(A&& a) { static_cast<A&&>(a).operator co_await(); };

template <class A>
concept CanConvertToTask = requires(A&& a) { static_cast<Task<int>>(static_cast<A&&>(a)); };

// An lvalue awaitable that can be copied.
class CopyableReady {
 public:
  explicit CopyableReady(int v) noexcept : v_(v) {}
  bool await_ready() const noexcept { return true; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  int await_resume() const noexcept { return v_; }

 private:
  int v_;
};

// CO-22: an lvalue Task (move-only) is rejected by Then's constraint
// instead of failing inside the coroutine; rvalue Tasks and copyable lvalue
// awaitables are accepted. A const rvalue OnAwaitable can be neither
// awaited nor converted (its const&& overloads are deleted).
static_assert(!CanPipeThen<Task<int>&>);
static_assert(!CanPipeThen<const Task<int>&>);
static_assert(!CanCallThen<Task<int>&>);
static_assert(CanPipeThen<Task<int>>);
static_assert(CanCallThen<Task<int>>);
static_assert(CanPipeThen<CopyableReady&>);
static_assert(CanCallThen<CopyableReady&>);
using OnInt = detail::OnAwaitable<InlineScheduler, Task<int>>;
static_assert(CanCoAwaitMember<OnInt>);
static_assert(!CanCoAwaitMember<const OnInt>);
static_assert(!CanCoAwaitMember<OnInt&>);
static_assert(CanConvertToTask<OnInt>);
static_assert(!CanConvertToTask<const OnInt>);

TEST(CoroThenForwarding, CopyableLvalueAwaitable) {
  CopyableReady r(5);
  EXPECT_EQ(*SyncWait(r | Then(AnyInt{})), 5);
  EXPECT_EQ(*SyncWait(Then(r, AnyInt{})), 5);
}

// CO-23: ~AsyncScope with active work terminates with a clear reason, in
// release builds too (not only through the debug assert).
TEST(CoroAsyncScopeDeathTest, DestroyedWithActiveWorkTerminates) {
  EXPECT_DEATH(
      {
        std::coroutine_handle<> parked;
        class Park {
         public:
          explicit Park(std::coroutine_handle<>* p) noexcept : p_(p) {}
          bool await_ready() const noexcept { return false; }
          IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> h) const noexcept { *p_ = h; }
          void await_resume() const noexcept {}

         private:
          std::coroutine_handle<>* p_;
        };
        auto park = [&]() -> Task<void> { co_await Park(&parked); };
        {
          AsyncScope scope;
          scope.spawn(park());
        }
      },
      "AsyncScope destroyed with active work");
}

// CO-24: StartDetached while the process-wide scope drains at exit (here:
// detached work starting more detached work) returns false and destroys the
// work without running it, as unifex drops work spawned on a stopped scope.
TEST(CoroStartDetachedDeathTest, ClosedScopeReportsDroppedWork) {
  EXPECT_EXIT(
      {
        static std::coroutine_handle<> parked;
        static std::atomic<bool> is_parked{false};
        class Park {
         public:
          Park() noexcept {}
          bool await_ready() const noexcept { return false; }
          // h is resumed by another thread once is_parked is set; parked and
          // is_parked are statics, not frame state.
          IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> h) const noexcept {
            parked = h;
            is_parked.store(true);
          }
          void await_resume() const noexcept {}
        };
        static Counts c;
        auto late = []() -> Task<void> {
          std::fputs("late work ran\n", stderr);
          co_return;
        };
        auto first = [late]() -> Task<void> {
          co_await Park();
          // Resumed by `resumer` below while exit() drains the scope.
          {
            Counter probe(&c);
            auto holder = [](Counter) -> Task<void> { co_return; };
            const bool started = StartDetached(holder(Counter(&c)));
            const bool started2 = StartDetached(late());
            std::fprintf(stderr, "started=%d started2=%d live=%d bad=%d\n", started, started2, c.Live() - 1,
                         c.bad_destroy.load());
          }
        };
        if (!StartDetached(first())) std::abort();
        while (!is_parked.load()) std::this_thread::yield();
        std::thread resumer([] {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          parked.resume();
        });
        resumer.detach();
        std::exit(0);
      },
      ::testing::ExitedWithCode(0), "started=0 started2=0 live=0 bad=0");
}

// CO-25: a thread_local read in a coroutine body after a co_await that
// resumed on another thread is that thread's instance (the compiler does not
// cache the TLS address across the suspension). Models IBMW's g_thread_ctx.
struct ThreadCtx {
  void* exe = nullptr;
  long n = 0;
};
thread_local ThreadCtx g_test_ctx;

class HopToNewThread {
 public:
  HopToNewThread() noexcept {}
  bool await_ready() const noexcept { return false; }
  IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> h) const {
    std::thread([h] { h.resume(); }).detach();
  }
  void await_resume() const noexcept {}
};

TEST(CoroThreadLocal, ReReadAfterResumeOnAnotherThread) {
  g_test_ctx.n = 100;
  std::atomic<long> fresh{0};
  auto body = [&]() -> Task<long> {
    long before = g_test_ctx.n;
    g_test_ctx.exe = &before;
    for (int i = 0; i < 200; ++i) {
      co_await HopToNewThread();
      g_test_ctx.n += 1;  // a fresh thread's slot: becomes 1
      if (g_test_ctx.n == 1 && g_test_ctx.exe == nullptr) fresh.fetch_add(1);
    }
    co_return before;
  };
  EXPECT_EQ(*SyncWait(body()), 100);
  EXPECT_EQ(fresh.load(), 200);
  EXPECT_EQ(g_test_ctx.n, 100) << "the calling thread's instance was not written after the first hop";
  g_test_ctx = {};
}

}  // namespace
}  // namespace ibmw::selfdev::coro
