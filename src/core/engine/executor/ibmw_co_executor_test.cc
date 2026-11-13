// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Integration tests for the coroutine binding of IbmwExecutor running on
// real IBMW executors. The IbmwExecutor tests (cpp/co/ibmw_context.h,
// details::MakeResumer) only build with IBMW_EXECUTOR_USE_SELFDEV_CORO; the
// coroutine RPC tests (IN-07, IN-09: CallAwaitable in cpp/service/rpc_handle.h,
// shared by both branches) and the cpp/co/thread_ctx_scope.h test also build
// with the unifex branch (IBMW_EXECUTOR_USE_SELFDEV_CORO=OFF). Nothing builds
// with stdexec.

#include "base/ibmw_config.h"

#if !defined(IBMW_EXECUTOR_USE_STDEXEC)

  #include <gtest/gtest.h>

  #include <atomic>
  #include <chrono>
  #include <coroutine>
  #include <future>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <stdexcept>
  #include <string>
  #include <thread>
  #include <utility>
  #include <vector>

  #include "cpp/co/ibmw_context.h"
  #include "cpp/co/on.h"
  #include "cpp/co/schedule.h"
  #include "cpp/co/sync_wait.h"
  #include "cpp/co/task.h"
  #include "cpp/co/thread_ctx_scope.h"
  #include "cpp/co/then.h"
  #include "cpp/service/rpc_handle.h"
  #include "engine/executor/executor_access.h"
  #include "engine/executor/parallel_executor.h"
  #include "engine/executor/thread_pool_executor.h"

namespace ibmw::runtime::core::executor {
namespace {

using namespace std::chrono_literals;
using ibmw::context::details::g_thread_ctx;

// An initialized and started executor plus the C ABI handle IBMW gives modules.
template <typename Executor>
class StartedExecutor {
 public:
  StartedExecutor(std::string_view name, const std::string& options)
      : access_(&executor_) {
    executor_.Initialize(name, YAML::Load(options));
    executor_.Start();
  }
  ~StartedExecutor() { executor_.Shutdown(); }

  StartedExecutor(const StartedExecutor&) = delete;
  StartedExecutor& operator=(const StartedExecutor&) = delete;

  Executor& Get() { return executor_; }
  ibmw::executor::ExecutorHandle Handle() const {
    return ibmw::executor::ExecutorHandle(access_.NativeHandle());
  }
  co::IbmwExecutor Co() const { return co::IbmwExecutor(Handle()); }

 private:
  Executor executor_;
  ExecutorAccess access_;
};

// Runs fn as a plain task on handle and waits for its result.
template <typename Fn>
auto RunOn(ibmw::executor::ExecutorHandle handle, Fn fn) {
  std::promise<decltype(fn())> promise;
  auto future = promise.get_future();
  handle.Dispatch([&promise, &fn]() { promise.set_value(fn()); });
  return future.get();
}

bool CurrentExeIs(ibmw::executor::ExecutorHandle handle) {
  return g_thread_ctx.exe.NativeHandle() == handle.NativeHandle();
}

  #if defined(IBMW_EXECUTOR_USE_SELFDEV_CORO)

TEST(IbmwCoExecutorTest, ScheduleResumesOnExecutorWithThreadContext) {
  StartedExecutor<ParallelExecutor> work("co_work", "{thread_num: 2}");
  const auto caller_exe = g_thread_ctx.exe.NativeHandle();

  auto body = [&]() -> co::Task<std::pair<bool, bool>> {
    co_await co::Schedule(work.Co());
    co_return std::make_pair(work.Get().IsInCurrentExecutor(), CurrentExeIs(work.Handle()));
  };

  auto result = co::SyncWait(body());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->first);
  EXPECT_TRUE(result->second);
  // The awaiting thread's own context is untouched.
  EXPECT_EQ(g_thread_ctx.exe.NativeHandle(), caller_exe);
}

TEST(IbmwCoExecutorTest, ResumerRestoresDispatchingWorkerContext) {
  StartedExecutor<ParallelExecutor> exe_a("co_a", "{thread_num: 1}");
  StartedExecutor<ParallelExecutor> exe_c("co_c", "{thread_num: 1}");
  StartedExecutor<ParallelExecutor> sentinel("co_sentinel", "{thread_num: 1}");

  // The single worker of exe_a carries a context unrelated to exe_a.
  RunOn(exe_a.Handle(), [&]() {
    g_thread_ctx.exe = sentinel.Handle();
    return true;
  });

  auto body = [&]() -> co::Task<std::pair<bool, bool>> {
    co_await co::Schedule(exe_a.Co());
    const bool on_a = CurrentExeIs(exe_a.Handle()) && exe_a.Get().IsInCurrentExecutor();
    // Suspending here makes the exe_a resumer return before the coroutine ends.
    co_await co::Schedule(exe_c.Co());
    const bool on_c = CurrentExeIs(exe_c.Handle()) && exe_c.Get().IsInCurrentExecutor();
    co_return std::make_pair(on_a, on_c);
  };

  auto result = co::SyncWait(body());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->first);
  EXPECT_TRUE(result->second);

  // Single-threaded executors run tasks in FIFO order, so these probes run
  // after the resumers have returned.
  EXPECT_TRUE(RunOn(exe_a.Handle(), [&]() { return CurrentExeIs(sentinel.Handle()); }));
  EXPECT_TRUE(RunOn(exe_c.Handle(), []() { return !g_thread_ctx.exe; }));
}

// A coroutine type (not ibmw::co::Task) whose unhandled_exception rethrows,
// so resuming it throws out of resume().
struct RethrowingCoroutine {
  struct promise_type {
    RethrowingCoroutine get_return_object() {
      return RethrowingCoroutine{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    std::suspend_always final_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() const { throw; }
  };
  std::coroutine_handle<promise_type> handle;
};

RethrowingCoroutine ThrowWhenResumed(bool* saw_exe) {
  *saw_exe = static_cast<bool>(g_thread_ctx.exe);
  throw std::runtime_error("resume throws");
  co_return;
}

// The resumer restores the worker's context even when resume() throws.
TEST(IbmwCoExecutorTest, ResumerRestoresContextWhenResumeThrows) {
  StartedExecutor<ParallelExecutor> exe_a("co_throw_a", "{thread_num: 1}");
  StartedExecutor<ParallelExecutor> sentinel("co_throw_sentinel", "{thread_num: 1}");

  const bool ok = RunOn(exe_a.Handle(), [&]() {
    g_thread_ctx.exe = sentinel.Handle();
    bool saw_exe = false;
    RethrowingCoroutine c = ThrowWhenResumed(&saw_exe);
    bool threw = false;
    try {
      co::details::MakeResumer(exe_a.Handle(), c.handle)();
    } catch (const std::runtime_error&) {
      threw = true;
    }
    c.handle.destroy();
    const bool restored = CurrentExeIs(sentinel.Handle());
    g_thread_ctx.exe = {};
    return threw && saw_exe && restored;
  });
  EXPECT_TRUE(ok);
}

TEST(IbmwCoExecutorTest, ScheduleAfterResumesNoEarlierOnTimedExecutor) {
  StartedExecutor<ThreadPoolExecutor> timed("co_timed", "{thread_num: 2}");
  ASSERT_TRUE(timed.Handle().SupportsTimedDispatch());
  constexpr auto kDelay = 30ms;

  auto body = [&]() -> co::Task<std::chrono::steady_clock::duration> {
    const auto start = std::chrono::steady_clock::now();
    co_await co::ScheduleAfter(timed.Co(), kDelay);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(timed.Get().IsInCurrentExecutor());
    EXPECT_TRUE(CurrentExeIs(timed.Handle()));
    co_return elapsed;
  };

  for (int ii = 0; ii < 3; ++ii) {
    auto elapsed = co::SyncWait(body());
    ASSERT_TRUE(elapsed.has_value());
    EXPECT_GE(*elapsed, kDelay);
  }
}

TEST(IbmwCoExecutorTest, ScheduleAtResumesNoEarlierOnTimedExecutor) {
  StartedExecutor<ThreadPoolExecutor> timed("co_timed_at", "{thread_num: 1}");
  auto handle = timed.Handle();

  auto body = [&]() -> co::Task<bool> {
    const auto deadline = handle.Now() + 25ms;
    co_await co::ScheduleAt(timed.Co(), deadline);
    EXPECT_TRUE(timed.Get().IsInCurrentExecutor());
    EXPECT_TRUE(CurrentExeIs(handle));
    co_return handle.Now() >= deadline;
  };

  auto reached = co::SyncWait(body());
  ASSERT_TRUE(reached.has_value());
  EXPECT_TRUE(*reached);

  // A deadline in the past resumes promptly, still on the executor.
  auto past = [&]() -> co::Task<bool> {
    co_await co::ScheduleAt(timed.Co(), handle.Now() - 1s);
    co_return CurrentExeIs(handle);
  };
  auto past_result = co::SyncWait(past());
  ASSERT_TRUE(past_result.has_value());
  EXPECT_TRUE(*past_result);
}

TEST(IbmwCoExecutorTest, OnThenRunsContinuationOnExecutor) {
  StartedExecutor<ParallelExecutor> work("co_on_then", "{thread_num: 2}");
  std::atomic_bool body_on_exe = false;
  std::atomic_bool then_on_exe = false;

  auto body = [&]() -> co::Task<int> {
    body_on_exe = work.Get().IsInCurrentExecutor() && CurrentExeIs(work.Handle());
    co_return 41;
  };

  auto result = co::SyncWait(co::On(work.Co(), body()) | co::Then([&](int v) {
                               then_on_exe = work.Get().IsInCurrentExecutor() &&
                                             CurrentExeIs(work.Handle());
                               return v + 1;
                             }));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 42);
  EXPECT_TRUE(body_on_exe.load());
  EXPECT_TRUE(then_on_exe.load());
}

TEST(IbmwCoExecutorTest, ExceptionPropagatesThroughCoAwait) {
  StartedExecutor<ParallelExecutor> work("co_throw", "{thread_num: 1}");
  StartedExecutor<ParallelExecutor> sentinel("co_throw_sentinel", "{thread_num: 1}");
  RunOn(work.Handle(), [&]() {
    g_thread_ctx.exe = sentinel.Handle();
    return true;
  });

  auto thrower = [&]() -> co::Task<int> {
    co_await co::Schedule(work.Co());
    throw std::runtime_error("boom on executor");
    co_return 0;
  };

  // Caught by an awaiting coroutine.
  auto catcher = [&]() -> co::Task<std::string> {
    try {
      co_await thrower();
    } catch (const std::runtime_error& e) {
      co_return std::string(e.what());
    }
    co_return std::string();
  };
  auto caught = co::SyncWait(catcher());
  ASSERT_TRUE(caught.has_value());
  EXPECT_EQ(*caught, "boom on executor");

  // Uncaught: rethrown by SyncWait on the waiting thread.
  EXPECT_THROW(co::SyncWait(thrower()), std::runtime_error);
  EXPECT_THROW(co::SyncWait(co::On(work.Co(), thrower()) | co::Then([](int v) { return v; })),
               std::runtime_error);

  // The worker survived and its context was restored after each resumption.
  EXPECT_TRUE(RunOn(work.Handle(), [&]() { return CurrentExeIs(sentinel.Handle()); }));
}

TEST(IbmwCoExecutorTest, ConcurrentHopsKeepThreadContextConsistent) {
  StartedExecutor<ParallelExecutor> exe_a("co_hop_a", "{thread_num: 4}");
  StartedExecutor<ParallelExecutor> exe_b("co_hop_b", "{thread_num: 4}");
  constexpr int kWaiters = 8;
  constexpr int kHops = 500;
  std::atomic_int mismatches = 0;
  std::atomic_int completed = 0;

  auto hopper = [&]() -> co::Task<int> {
    int hops = 0;
    for (int ii = 0; ii < kHops; ++ii) {
      co_await co::Schedule(exe_a.Co());
      if (!CurrentExeIs(exe_a.Handle()) || !exe_a.Get().IsInCurrentExecutor()) ++mismatches;
      co_await co::Schedule(exe_b.Co());
      if (!CurrentExeIs(exe_b.Handle()) || !exe_b.Get().IsInCurrentExecutor()) ++mismatches;
      hops += 2;
    }
    co_return hops;
  };

  std::vector<std::thread> waiters;
  for (int ii = 0; ii < kWaiters; ++ii) {
    waiters.emplace_back([&]() {
      auto hops = co::SyncWait(hopper());
      if (hops && *hops == 2 * kHops) ++completed;
    });
  }
  for (auto& t : waiters) t.join();

  EXPECT_EQ(mismatches.load(), 0);
  EXPECT_EQ(completed.load(), kWaiters);
}

  #endif  // IBMW_EXECUTOR_USE_SELFDEV_CORO

// ThreadCtxScope (used by MakeResumer and by CallAwaitable's resumer) sets
// only exe and restores the thread's whole previous context (exe, ctx_ptr,
// active_rpc_context), also when the scope is left by an exception. The body
// inside each scope clears ctx_ptr and active_rpc_context, so restoring only
// exe would fail.
TEST(IbmwCoExecutorTest, ThreadCtxScopeSetsExeAndRestoresContext) {
  StartedExecutor<ParallelExecutor> exe_a("co_scope_a", "{thread_num: 1}");
  StartedExecutor<ParallelExecutor> sentinel("co_scope_sentinel", "{thread_num: 1}");

  const bool ok = RunOn(exe_a.Handle(), [&]() {
    // A ctx_ptr identified by its owner (no Context is needed: the weak_ptr
    // is only compared, never locked and dereferenced).
    const auto owner = std::make_shared<int>(0);
    const std::shared_ptr<ibmw::context::Context> ctx_marker(owner, nullptr);
    const auto is_previous = [&]() {
      const auto& ctx = g_thread_ctx.ctx_ptr;
      return CurrentExeIs(sentinel.Handle()) && !ctx.owner_before(ctx_marker) && !ctx_marker.owner_before(ctx) &&
             !ctx.expired() && g_thread_ctx.active_rpc_context.has_value();
    };
    g_thread_ctx.ctx_ptr = ctx_marker;
    g_thread_ctx.exe = sentinel.Handle();
    g_thread_ctx.active_rpc_context = ibmw::rpc::ContextRef();
    const bool before = is_previous();
    bool inside = false;
    {
      co::details::ThreadCtxScope scope(exe_a.Handle());
      inside = CurrentExeIs(exe_a.Handle()) && !g_thread_ctx.ctx_ptr.expired() &&
               g_thread_ctx.active_rpc_context.has_value();
      g_thread_ctx.ctx_ptr.reset();
      g_thread_ctx.active_rpc_context.reset();
    }
    const bool restored = is_previous();
    bool threw = false;
    try {
      co::details::ThreadCtxScope scope(exe_a.Handle());
      g_thread_ctx.ctx_ptr.reset();
      g_thread_ctx.active_rpc_context.reset();
      throw std::runtime_error("leave the scope");
    } catch (const std::runtime_error&) {
      threw = true;
    }
    const bool restored_after_throw = is_previous();
    g_thread_ctx = {};
    return before && inside && restored && threw && restored_after_throw;
  });
  EXPECT_TRUE(ok);
}

// IN-07: a coroutine RPC call (CoAccessBase::Call -> CallAwaitable) against a
// fake service handle. Guards the GCC 11/12 aggregate co_await double
// destruction in rpc_handle.h (shared_ptr members released twice; ASAN:
// heap-use-after-free) and the await_suspend rule (the response may resume
// the coroutine on another thread before Call() returns). Completion is
// either inline inside call() or from a separate thread; either way the
// coroutine must resume on a worker of the executor it called from.
class FakeRpcService {
 public:
  explicit FakeRpcService(bool inline_completion) : inline_completion_(inline_completion) {}
  ~FakeRpcService() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& t : threads_) t.join();
  }

  ibmw::rpc::ServiceRef Ref() const { return ibmw::rpc::ServiceRef(&base_); }
  int Calls() const { return calls_.load(); }

 private:
  // Answers rsp = 2 * req with status OK.
  static void Call(void* ctx, ibmw_string_view_t, const ibmw_service_context_base_t*,
                   const void* req_ptr, void* rsp_ptr, ibmw_function_base_t* callback_base) {
    auto* self = static_cast<FakeRpcService*>(ctx);
    self->calls_.fetch_add(1);
    ibmw::rpc::ClientCallback callback(callback_base);
    auto complete = [req_ptr, rsp_ptr, callback = std::move(callback)]() {
      *static_cast<int*>(rsp_ptr) = 2 * *static_cast<const int*>(req_ptr);
      callback(static_cast<uint32_t>(IBMW_RPC_STATUS_OK));
    };
    if (self->inline_completion_) {
      complete();
      return;
    }
    std::lock_guard<std::mutex> lock(self->mu_);
    self->threads_.emplace_back(std::move(complete));
  }

  const bool inline_completion_;
  std::atomic_int calls_ = 0;
  std::mutex mu_;
  std::vector<std::thread> threads_;
  ibmw_service_handle_base_t base_{
      .register_service_func = nullptr,
      .register_client_func = nullptr,
      .call = &FakeRpcService::Call,
      .merge_server_context_to_client_context = nullptr,
      .ctx = this};
};

class DoubleClient : public ibmw::rpc::CoAccessBase {
 public:
  explicit DoubleClient(ibmw::rpc::ServiceRef ref) : CoAccessBase(ref, "test", "double") {}
  co::Task<ibmw::rpc::Status> Double(const int& req, int& rsp) {
    return Call("test/double/Double", ibmw::rpc::ContextRef(), req, rsp);
  }
};

TEST(IbmwCoExecutorTest, CoroutineRpcCallResumesOnCallerExecutor) {
  StartedExecutor<ParallelExecutor> work("co_rpc_work", "{thread_num: 4}");
  constexpr int kWaiters = 4;
  constexpr int kCallsPerWaiter = 200;

  for (const bool inline_completion : {false, true}) {
    FakeRpcService service(inline_completion);
    DoubleClient client(service.Ref());
    std::atomic_int bad_status = 0;
    std::atomic_int bad_rsp = 0;
    std::atomic_int bad_exe = 0;
    std::atomic_int ok = 0;

    auto caller = [&](int base) -> co::Task<int> {
      int good = 0;
      for (int ii = 0; ii < kCallsPerWaiter; ++ii) {
        co_await co::Schedule(work.Co());  // resume_exe is the calling executor
        int rsp = 0;
        const int req = base + ii;
        const ibmw::rpc::Status status = co_await client.Double(req, rsp);
        const bool status_ok = status.OK();
        const bool rsp_ok = rsp == 2 * req;
        // CallAwaitable resumes on a worker of the calling executor with
        // g_thread_ctx.exe set to it (IN-09 covers what depends on that).
        const bool exe_ok = work.Get().IsInCurrentExecutor() && CurrentExeIs(work.Handle());
        if (!status_ok) ++bad_status;
        if (!rsp_ok) ++bad_rsp;
        if (!exe_ok) ++bad_exe;
        if (status_ok && rsp_ok && exe_ok) ++good;
      }
      co_return good;
    };

    std::vector<std::thread> waiters;
    for (int ww = 0; ww < kWaiters; ++ww) {
      waiters.emplace_back([&, ww]() {
        auto good = co::SyncWait(caller(ww * 100000));
        if (good) ok.fetch_add(*good);
      });
    }
    for (auto& t : waiters) t.join();

    EXPECT_EQ(bad_status.load(), 0) << "inline_completion=" << inline_completion;
    EXPECT_EQ(bad_rsp.load(), 0) << "inline_completion=" << inline_completion;
    EXPECT_EQ(bad_exe.load(), 0) << "inline_completion=" << inline_completion;
    EXPECT_EQ(ok.load(), kWaiters * kCallsPerWaiter) << "inline_completion=" << inline_completion;
    EXPECT_EQ(service.Calls(), kWaiters * kCallsPerWaiter) << "inline_completion=" << inline_completion;
  }
}

// IN-09: back-to-back coroutine RPC calls with no Schedule in between, the
// pattern of ibmw-template's rpc_client_module and of the ibmw_gen RPC client
// loop. The second call takes its resume executor from g_thread_ctx.exe on
// the thread the first call resumed on; CallAwaitable used to resume with a
// plain Dispatch() that left it unset, so the second call failed with
// "Coroutine RPC resume executor is not set". Each call must resume on the
// calling executor with g_thread_ctx.exe set, code that depends on it (here
// co::Schedule on the executor read back from g_thread_ctx.exe) must work
// after the calls, and the worker's own context must be restored afterwards.
struct BackToBackResult {
  int calls_ok = 0;         // status OK and rsp == 2 * req
  int resumed_ok = 0;       // IsInCurrentExecutor() and g_thread_ctx.exe == calling executor
  bool rescheduled = false; // co::Schedule(IbmwExecutor(g_thread_ctx.exe)) landed on it
  std::string error;
};

co::Task<BackToBackResult> BackToBackCalls(DoubleClient& client, ParallelExecutor& work,
                                           ibmw::executor::ExecutorHandle work_handle, int calls) {
  BackToBackResult result;
  try {
    for (int ii = 1; ii <= calls; ++ii) {
      int rsp = 0;
      const ibmw::rpc::Status status = co_await client.Double(ii, rsp);
      if (status.OK() && rsp == 2 * ii) ++result.calls_ok;
      if (work.IsInCurrentExecutor() && CurrentExeIs(work_handle)) ++result.resumed_ok;
    }
    if (CurrentExeIs(work_handle)) {
      co_await co::Schedule(co::IbmwExecutor(g_thread_ctx.exe));
      result.rescheduled = work.IsInCurrentExecutor() && CurrentExeIs(work_handle);
    }
  } catch (const std::exception& e) {
    result.error = e.what();
  }
  co_return result;
}

TEST(IbmwCoExecutorTest, BackToBackRpcCallsKeepResumeExecutor) {
  StartedExecutor<ParallelExecutor> work("co_rpc_b2b", "{thread_num: 2}");
  constexpr int kCalls = 3;
  const auto worker_exe_before = RunOn(work.Handle(), []() { return g_thread_ctx.exe.NativeHandle(); });

  for (const bool inline_completion : {false, true}) {
    for (const bool under_on : {false, true}) {
      FakeRpcService service(inline_completion);
      DoubleClient client(service.Ref());
      const std::string where = std::string("inline_completion=") + (inline_completion ? "1" : "0") +
                                " under_on=" + (under_on ? "1" : "0");

      std::optional<BackToBackResult> result;
      if (under_on) {
        // co::On sets g_thread_ctx.exe for the first call.
        result = co::SyncWait(co::On(work.Co(), BackToBackCalls(client, work.Get(), work.Handle(), kCalls)));
      } else {
        auto body = [&]() -> co::Task<BackToBackResult> {
          co_await co::Schedule(work.Co());
          co_return co_await BackToBackCalls(client, work.Get(), work.Handle(), kCalls);
        };
        result = co::SyncWait(body());
      }

      ASSERT_TRUE(result.has_value()) << where;
      EXPECT_EQ(result->error, "") << where;
      EXPECT_EQ(result->calls_ok, kCalls) << where;
      EXPECT_EQ(result->resumed_ok, kCalls) << where;
      EXPECT_TRUE(result->rescheduled) << where;
      EXPECT_EQ(service.Calls(), kCalls) << where;
    }
  }
  // The resumer restores the worker's own context after each resume.
  EXPECT_EQ(RunOn(work.Handle(), []() { return g_thread_ctx.exe.NativeHandle(); }), worker_exe_before);
}

}  // namespace
}  // namespace ibmw::runtime::core::executor

#endif  // !IBMW_EXECUTOR_USE_STDEXEC
