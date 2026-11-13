// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ibmw_selfdev/coro/task.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "ibmw_selfdev/coro/export.h"
#include "ibmw_selfdev/coro/on.h"
#include "ibmw_selfdev/coro/sync_wait.h"
#include "ibmw_selfdev/coro/then.h"
#include "thread_pool_scheduler.h"

namespace ibmw::selfdev::coro {
namespace {

Task<int> Value(int v) { co_return v; }

Task<int> Add(int a, int b) { co_return co_await Value(a) + co_await Value(b); }

Task<int> Outer() { co_return co_await Add(20, 22); }

Task<void> Throws() {
  throw std::runtime_error("boom");
  co_return;
}

// CO-01
TEST(CoroTask, IsLazy) {
  int ran = 0;
  auto body = [&]() -> Task<void> {
    ++ran;
    co_return;
  };
  auto t = body();
  EXPECT_EQ(ran, 0);
  SyncWait(std::move(t));
  EXPECT_EQ(ran, 1);
}

// CO-02
TEST(CoroTask, ReturnsValue) {
  auto r = SyncWait(Value(42));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 42);
}

// CO-03
TEST(CoroTask, VoidTaskYieldsUnit) {
  auto body = []() -> Task<void> { co_return; };
  std::optional<Unit> r = SyncWait(body());
  EXPECT_TRUE(r.has_value());
}

// CO-04
TEST(CoroTask, NestedAwait) { EXPECT_EQ(*SyncWait(Outer()), 42); }

// CO-05
TEST(CoroTask, ExceptionPropagates) {
  auto catcher = []() -> Task<int> {
    try {
      co_await Throws();
    } catch (const std::runtime_error& e) {
      co_return std::string(e.what()) == "boom" ? 1 : 0;
    }
    co_return -1;
  };
  EXPECT_EQ(*SyncWait(catcher()), 1);
  EXPECT_THROW(SyncWait(Throws()), std::runtime_error);
}

// CO-06
TEST(CoroTask, MoveOnlyResult) {
  auto body = []() -> Task<std::unique_ptr<int>> { co_return std::make_unique<int>(7); };
  auto r = SyncWait(body());
  ASSERT_TRUE(r.has_value());
  ASSERT_NE(*r, nullptr);
  EXPECT_EQ(**r, 7);
}

struct DtorCounter {
  explicit DtorCounter(int* c) : count(c) {}
  DtorCounter(DtorCounter&& o) noexcept : count(std::exchange(o.count, nullptr)) {}
  ~DtorCounter() {
    if (count) ++*count;
  }
  int* count;
};

// CO-07
TEST(CoroTask, UnstartedTaskDestroysFrame) {
  int destroyed = 0;
  int ran = 0;
  {
    auto body = [](DtorCounter, int* r) -> Task<void> {
      ++*r;
      co_return;
    };
    auto t = body(DtorCounter(&destroyed), &ran);
    EXPECT_EQ(destroyed, 0);
  }
  EXPECT_EQ(ran, 0);
  EXPECT_EQ(destroyed, 1);
}

Task<int> One() { co_return 1; }

// CO-08
TEST(CoroTask, DeepSynchronousLoopDoesNotOverflow) {
  auto loop = []() -> Task<long> {
    long sum = 0;
    for (int i = 0; i < 1000000; ++i) sum += co_await One();
    co_return sum;
  };
  EXPECT_EQ(*SyncWait(loop()), 1000000);
}

// Resumes the awaiter from another thread, like rpc_handle.h's CallAwaitable.
// Awaiters used as `co_await X{...}` temporaries get a user-declared
// constructor: GCC 11/12 destroys the members of an aggregate-initialized
// co_await operand twice (see awaiter_rules_test.cc).
struct OtherThreadAwaitable {
  explicit OtherThreadAwaitable(int v) noexcept : value(v) {}
  int value;
  bool await_ready() const noexcept { return false; }
  // noinline: the std::thread temporary must live on this stack, not in the
  // frame the new thread may already have destroyed.
  IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> h) const {
    std::thread([h] { h.resume(); }).detach();
  }
  int await_resume() const noexcept { return value; }
};

// CO-09
TEST(CoroTask, AwaitsCustomAwaitable) {
  auto body = []() -> Task<int> {
    int a = co_await OtherThreadAwaitable(40);
    int b = co_await OtherThreadAwaitable(2);
    co_return a + b;
  };
  EXPECT_EQ(*SyncWait(body()), 42);
}

// CO-10
TEST(CoroTask, MoveConstructAndAssign) {
  int destroyed = 0;
  auto body = [](DtorCounter) -> Task<int> { co_return 5; };
  Task<int> a = body(DtorCounter(&destroyed));
  Task<int> b(std::move(a));
  EXPECT_FALSE(a.Valid());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(b.Valid());

  Task<int> c = body(DtorCounter(&destroyed));
  c = std::move(b);  // old frame of c is destroyed
  EXPECT_EQ(destroyed, 1);
  EXPECT_FALSE(b.Valid());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(*SyncWait(std::move(c)), 5);
  EXPECT_EQ(destroyed, 2);
}

// Resumes the awaiter on another thread, then keeps await_suspend (and so
// the parent's Start()) busy for a moment, so the child usually completes
// before Start() returns (before the parent counts as suspended).
struct LateStartAwaitable {
  explicit LateStartAwaitable(std::thread::id* r) noexcept : resumer(r) {}
  std::thread::id* resumer;
  bool await_ready() const noexcept { return false; }
  // Touches nothing of the awaiter or the frame after publishing h.
  IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> h) const {
    std::thread([h] { h.resume(); }).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  void await_resume() const noexcept { *resumer = detail::CurrentThreadId(); }
};

// CO-11: the child completes on another thread while the parent's Start() is
// still running -> the parent must continue on the child's thread (as with
// unifex), not inline on the thread that started it. Found as a rare
// CoroThen.PipeForm failure (Clang Release, 1 in 20 runs).
TEST(CoroTask, AsyncCompletionDuringStartResumesOnCompletingThread) {
  for (int i = 0; i < 50; ++i) {
    std::thread::id child_thread;
    std::thread::id parent_thread;
    auto child = [&]() -> Task<int> {
      co_await LateStartAwaitable(&child_thread);
      co_return 7;
    };
    auto parent = [&]() -> Task<int> {
      int v = co_await child();
      parent_thread = detail::CurrentThreadId();
      co_return v;
    };
    EXPECT_EQ(*SyncWait(parent()), 7);
    EXPECT_NE(child_thread, detail::CurrentThreadId());
    EXPECT_EQ(parent_thread, child_thread);
  }
}

// CO-11b: same contract through On(pool, x) | Then(f): f always runs on the
// pool thread (stress version of TH-03).
TEST(CoroTask, OnThenAlwaysContinuesOnPool) {
  test::ThreadPool pool(2);
  for (int i = 0; i < 20000; ++i) {
    bool on_worker = false;
    auto sync_child = []() -> Task<int> { co_return 1; };
    auto chained = On(pool.GetScheduler(), sync_child()) | Then([&](int x) {
                     on_worker = pool.IsWorkerThread();
                     return x;
                   });
    ASSERT_EQ(*SyncWait(std::move(chained)), 1);
    ASSERT_TRUE(on_worker) << "iteration " << i;
  }
}

// Eager fire-and-forget coroutine: returns to the caller at its first
// suspension, so once the call returns every frame below it has suspended.
struct EagerDriver {
  struct promise_type {
    EagerDriver get_return_object() const noexcept { return {}; }
    std::suspend_never initial_suspend() const noexcept { return {}; }
    std::suspend_never final_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() const noexcept { std::terminate(); }
  };
};

// Parks the awaiting coroutine; the test resumes it later.
struct ParkAwaitable {
  explicit ParkAwaitable(std::coroutine_handle<>* p) noexcept : parked(p) {}
  std::coroutine_handle<>* parked;
  bool await_ready() const noexcept { return false; }
  IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> h) const noexcept { *parked = h; }
  void await_resume() const noexcept {}
};

// CO-12: the parent has fully suspended (Start() returned true) before
// the child is resumed on another thread -> the parent continues on that
// thread through the final awaiter.
TEST(CoroTask, HandoffParentSuspendsFirst) {
  for (int i = 0; i < 20; ++i) {
    std::coroutine_handle<> parked;
    std::thread::id parent_thread;
    std::atomic<bool> done{false};
    auto child = [&]() -> Task<int> {
      co_await ParkAwaitable(&parked);
      co_return 7;
    };
    auto parent = [&]() -> Task<int> {
      int v = co_await child();
      parent_thread = detail::CurrentThreadId();
      co_return v;
    };
    int result = 0;
    auto drive = [&]() -> EagerDriver {
      result = co_await parent();
      done.store(true);
    };
    drive();  // returns once child, parent and driver are all suspended
    ASSERT_TRUE(parked);
    EXPECT_FALSE(done.load());
    std::thread::id resumer;
    std::thread t([&] {
      resumer = detail::CurrentThreadId();
      parked.resume();
    });
    t.join();
    EXPECT_TRUE(done.load());
    EXPECT_EQ(result, 7);
    EXPECT_EQ(parent_thread, resumer);
  }
}

// Resumes the awaiting coroutine inline from inside await_suspend (legal:
// the coroutine is already suspended there).
struct ResumeInlineAwaitable {
  ResumeInlineAwaitable() noexcept {}
  bool await_ready() const noexcept { return false; }
  // The resumed coroutine may finish and destroy the frame holding *this.
  IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> h) const { h.resume(); }
  void await_resume() const noexcept {}
};

// CO-14: a child that completes re-entrantly inside its awaitable's
// await_suspend still counts as synchronous: 1M iterations do not grow the
// stack (GCC -O0 included).
TEST(CoroTask, ReentrantInlineResumeDoesNotOverflow) {
  auto child = []() -> Task<int> {
    co_await ResumeInlineAwaitable();
    co_return 1;
  };
  auto loop = [&]() -> Task<long> {
    long sum = 0;
    for (int i = 0; i < 1000000; ++i) sum += co_await child();
    co_return sum;
  };
  EXPECT_EQ(*SyncWait(loop()), 1000000);
}

// CO-15: the child completes later on the same thread that started it (one
// worker thread): Start() has returned, so the parent must be resumed by the
// completion, on that thread, without hanging.
TEST(CoroTask, LaterCompletionOnStartingThread) {
  test::ThreadPool pool(1);
  auto child = [&]() -> Task<std::thread::id> {
    co_await pool.GetScheduler().schedule();
    co_return detail::CurrentThreadId();
  };
  auto parent = [&]() -> Task<bool> {
    co_await pool.GetScheduler().schedule();
    const std::thread::id worker = detail::CurrentThreadId();
    bool ok = true;
    for (int i = 0; i < 10000; ++i) {
      ok = ok && (co_await child()) == worker;
      ok = ok && detail::CurrentThreadId() == worker;
    }
    co_return ok;
  };
  EXPECT_TRUE(*SyncWait(parent()));
}

// One-slot handoff of a coroutine handle between threads. Notifies under the
// lock: the taker may destroy the mailbox as soon as it sees the handle.
class HandleMailbox {
 public:
  void Put(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(mu_);
    h_ = h;
    cv_.notify_one();
  }

  std::coroutine_handle<> Take() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return static_cast<bool>(h_); });
    return std::exchange(h_, {});
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::coroutine_handle<> h_;
};

// nested: hands the awaiting coroutine to a new thread, then keeps this
// thread inside await_suspend (and so inside the parent's Start()) until it
// has run one coroutine taken from `mb`. Otherwise: posts the awaiting
// coroutine to `mb`.
struct HandoffAwaitable {
  HandoffAwaitable(HandleMailbox* m, void** f, bool n) noexcept : mb(m), frame(f), nested(n) {}
  HandleMailbox* mb;
  void** frame;
  bool nested;
  bool await_ready() const noexcept { return false; }
  // noinline: `box` must live on this stack, not in the (possibly already
  // destroyed) coroutine frame (Clang < 19 may place inlined locals there).
  IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> h) const {
    *frame = h.address();
    if (!nested) {
      mb->Put(h);
      return;
    }
    // This awaitable lives in the frame, which may be destroyed once h runs.
    HandleMailbox* const box = mb;
    std::thread([h] { h.resume(); }).detach();
    box->Take().resume();
  }
  void await_resume() const noexcept {}
};

// Same coroutine for both children so their frames share a size class and
// the allocator tends to hand the first one's memory to the second. (A single
// co_await: Clang 14 crashes splitting a coroutine with one in each branch.)
Task<int> HandoffChild(HandleMailbox* mb, void** frame, bool nested) {
  co_await HandoffAwaitable(mb, frame, nested);
  co_return nested ? 1 : 2;
}

// CO-16: frame address reuse. Child 1 starts on thread A and finishes on
// thread B while A stays inside Start(child 1); the parent continues on B and
// starts child 2 (often at child 1's old address), which finishes on A. Child
// 2 is not child 1's inline completion: the parent must be resumed once, by
// child 2's completion, with the right results.
TEST(CoroTask, ReusedFrameAddressIsNotInlineCompletion) {
  int reused = 0;
  for (int i = 0; i < 200; ++i) {
    HandleMailbox mb;
    void* first = nullptr;
    void* second = nullptr;
    auto parent = [&]() -> Task<int> {
      int a = co_await HandoffChild(&mb, &first, true);
      int b = co_await HandoffChild(&mb, &second, false);
      co_return a * 10 + b;
    };
    ASSERT_EQ(*SyncWait(parent()), 12) << "iteration " << i;
    if (first == second) ++reused;
  }
  // Informational: sanitizer allocators quarantine freed memory.
  RecordProperty("reused_frames", reused);
}

// CO-13: co_await on a moved-from Task terminates (release builds too).
TEST(CoroTaskDeathTest, AwaitInvalidTaskTerminates) {
  EXPECT_DEATH(
      {
        Task<int> a = Value(1);
        Task<int> b = std::move(a);
        auto body = [&]() -> Task<int> { co_return co_await std::move(a); };  // NOLINT(bugprone-use-after-move)
        (void)SyncWait(body());
      },
      "");
}

}  // namespace
}  // namespace ibmw::selfdev::coro
