// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests: blocking / future RPC accessors must tolerate a provider
// that completes the same call more than once.

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include "cpp/co/sync_wait.h"
#include "cpp/executor/executor.h"
#include "cpp/service/rpc_handle.h"

namespace ibmw::rpc {
namespace {

// Provider that completes every call twice, synchronously.
struct DoubleCompleteService {
  static bool RegisterService(void*, ibmw_string_view_t, const void*, const ibmw_type_support_base_t*,
                              const ibmw_type_support_base_t*, ibmw_function_base_t*) { return true; }
  static bool RegisterClient(void*, ibmw_string_view_t, const void*, const ibmw_type_support_base_t*,
                             const ibmw_type_support_base_t*) { return true; }
  static void Call(void*, ibmw_string_view_t, const ibmw_service_context_base_t*, const void*, void*,
                   ibmw_function_base_t* callback) {
    ClientCallback cb(callback);
    cb(7);
    cb(9);  // duplicate completion must be ignored, not terminate
  }
  static void Merge(void*, const ibmw_service_context_base_t*, const ibmw_service_context_base_t*) {}

  ibmw_service_handle_base_t base{
      .register_service_func = &RegisterService,
      .register_client_func = &RegisterClient,
      .call = &Call,
      .merge_server_context_to_client_context = &Merge,
      .ctx = nullptr};
};

struct SyncProbe : SyncAccessBase {
  using SyncAccessBase::SyncAccessBase;
  Status Run() {
    int req = 0, rsp = 0;
    return Call("json:/svc/fn", ContextRef(), req, rsp);
  }
};

struct FutureProbe : FutureAccessBase {
  using FutureAccessBase::FutureAccessBase;
  std::future<Status> Run(int& req, int& rsp) { return Call("json:/svc/fn", ContextRef(), req, rsp); }
};

TEST(RpcHandle, SyncCallIgnoresDuplicateCompletion) {
  DoubleCompleteService svc;
  SyncProbe probe(ServiceRef(&svc.base), "json", "svc");
  EXPECT_EQ(probe.Run().Code(), 7u);
}

TEST(RpcHandle, FutureCallIgnoresDuplicateCompletion) {
  DoubleCompleteService svc;
  FutureProbe probe(ServiceRef(&svc.base), "json", "svc");
  int req = 0, rsp = 0;
  EXPECT_EQ(probe.Run(req, rsp).get().Code(), 7u);
}

// Coroutine RPC (API-013/API-014). The provider either answers inline or
// drops the callback without calling it.
struct CoService {
  enum class Mode { kAnswer, kDrop };
  static inline Mode mode = Mode::kAnswer;

  static void Call(void*, ibmw_string_view_t, const ibmw_service_context_base_t*, const void*, void*,
                   ibmw_function_base_t* callback) {
    ClientCallback cb(callback);
    if (mode == Mode::kAnswer) cb(5);
  }

  ibmw_service_handle_base_t base{
      .register_service_func = &DoubleCompleteService::RegisterService,
      .register_client_func = &DoubleCompleteService::RegisterClient,
      .call = &Call,
      .merge_server_context_to_client_context = &DoubleCompleteService::Merge,
      .ctx = nullptr};
};

// Runs every dispatched task inline on the calling thread.
struct InlineExecutor {
  static ibmw_string_view_t Type(void*) { return {"inline", 6}; }
  static bool True(void*) { return true; }
  static bool False(void*) { return false; }
  static void Dispatch(void* ctx, ibmw_function_base_t* task) {
    ++*static_cast<int*>(ctx);
    ibmw::executor::Task{task}();
  }
  static uint64_t Now(void*) { return 0; }
  static void DispatchAt(void*, uint64_t, ibmw_function_base_t*) {}

  int dispatched = 0;
  ibmw_executor_base_t base{
      .type = &Type,
      .name = &Type,
      .is_thread_safe = &True,
      .is_in_current_executor = &True,
      .supports_timed_dispatch = &False,
      .dispatch = &Dispatch,
      .now = &Now,
      .dispatch_at_ns = &DispatchAt,
      .ctx = &dispatched};
};

// Runs every dispatched task on a new thread, after Dispatch returns, like a
// real executor resuming the caller later.
struct ThreadExecutor {
  static void Dispatch(void* ctx, ibmw_function_base_t* task) {
    static_cast<ThreadExecutor*>(ctx)->threads.emplace_back([t = ibmw::executor::Task{task}]() mutable { t(); });
  }
  ~ThreadExecutor() {
    for (auto& thread : threads) thread.join();
  }

  std::vector<std::thread> threads;
  ibmw_executor_base_t base{
      .type = &InlineExecutor::Type,
      .name = &InlineExecutor::Type,
      .is_thread_safe = &InlineExecutor::True,
      .is_in_current_executor = &InlineExecutor::True,
      .supports_timed_dispatch = &InlineExecutor::False,
      .dispatch = &Dispatch,
      .now = &InlineExecutor::Now,
      .dispatch_at_ns = &InlineExecutor::DispatchAt,
      .ctx = this};
};

struct CoProbe : CoAccessBase {
  using CoAccessBase::CoAccessBase;
  ibmw::co::Task<Status> Run() {
    int req = 0, rsp = 0;
    co_return co_await Call("json:/svc/fn", ContextRef(), req, rsp);
  }
};

Status RunCo(CoService::Mode mode, ibmw_executor_base_t* exe) {
  CoService::mode = mode;
  CoService svc;
  CoProbe probe(ServiceRef(&svc.base), "json", "svc");
  ibmw::context::details::ScopedThreadContextRestore restore;
  ibmw::context::details::g_thread_ctx.exe = ibmw::executor::ExecutorHandle(exe);
  auto result = ibmw::co::SyncWait(probe.Run());
  return result ? *result : Status(IBMW_RPC_STATUS_UNKNOWN);
}

TEST(RpcHandle, CoCallResumesWithAnswer) {
  InlineExecutor exe;
  EXPECT_EQ(RunCo(CoService::Mode::kAnswer, &exe.base).Code(), 5u);
  EXPECT_EQ(exe.dispatched, 1);
}

TEST(RpcHandle, CoCallResumesWhenCallbackIsDropped) {
  InlineExecutor exe;
  EXPECT_EQ(RunCo(CoService::Mode::kDrop, &exe.base).Code(),
            static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
  EXPECT_EQ(exe.dispatched, 1);
}

// The context Call creates for a caller without one lives until Call ends
// (a braced awaitable temporary released it twice under GCC 11; seen by ASan).
TEST(RpcHandle, CoCallResumedOnAnotherThread) {
  ThreadExecutor exe;
  EXPECT_EQ(RunCo(CoService::Mode::kAnswer, &exe.base).Code(), 5u);
  EXPECT_EQ(RunCo(CoService::Mode::kDrop, &exe.base).Code(),
            static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
  EXPECT_EQ(exe.threads.size(), 2u);
}

TEST(RpcHandle, CoCallOffExecutorFailsWithoutThrowing) {
  Status status;
  EXPECT_NO_THROW(status = RunCo(CoService::Mode::kAnswer, nullptr));
  EXPECT_EQ(status.Code(), static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_INVALID_CONTEXT));
}

// API-012: the shared_ptr overloads keep req/rsp alive after the caller drops
// them. The provider holds the call and answers later.
struct DeferredService {
  struct Pending {
    const void* req;
    void* rsp;
    ClientCallback callback;
  };
  static inline std::deque<Pending> pending;

  static void Call(void*, ibmw_string_view_t, const ibmw_service_context_base_t*, const void* req, void* rsp,
                   ibmw_function_base_t* callback) {
    pending.push_back(Pending{req, rsp, ClientCallback(callback)});
  }
  // Writes req + 1 into rsp, then completes with code 3.
  static void AnswerAll() {
    for (auto& call : pending) {
      *static_cast<int*>(call.rsp) = *static_cast<const int*>(call.req) + 1;
      call.callback(3);
    }
    pending.clear();
  }

  ibmw_service_handle_base_t base{
      .register_service_func = &DoubleCompleteService::RegisterService,
      .register_client_func = &DoubleCompleteService::RegisterClient,
      .call = &Call,
      .merge_server_context_to_client_context = &DoubleCompleteService::Merge,
      .ctx = nullptr};
};

struct SharedAsyncProbe : AsyncAccessBase {
  using AsyncAccessBase::AsyncAccessBase;
  void Run(std::shared_ptr<const int> req, std::shared_ptr<int> rsp, std::function<void(Status)>&& callback) {
    Call("json:/svc/fn", ContextRef(), std::move(req), std::move(rsp), std::move(callback));
  }
};

struct SharedFutureProbe : FutureAccessBase {
  using FutureAccessBase::FutureAccessBase;
  std::future<Status> Run(std::shared_ptr<const int> req, std::shared_ptr<int> rsp) {
    return Call("json:/svc/fn", ContextRef(), std::move(req), std::move(rsp));
  }
};

TEST(RpcHandle, AsyncSharedCallKeepsRequestAndResponseAlive) {
  DeferredService svc;
  SharedAsyncProbe probe(ServiceRef(&svc.base), "json", "svc");
  auto req = std::make_shared<const int>(41);
  auto rsp = std::make_shared<int>(0);
  std::weak_ptr<const int> req_weak = req;
  std::weak_ptr<int> rsp_weak = rsp;
  int answer = 0;
  uint32_t code = 0;
  probe.Run(std::move(req), std::move(rsp), [&answer, &code, rsp_weak](Status status) {
    code = status.Code();
    answer = *rsp_weak.lock();
  });
  ASSERT_EQ(DeferredService::pending.size(), 1u);
  EXPECT_FALSE(req_weak.expired());
  EXPECT_FALSE(rsp_weak.expired());

  DeferredService::AnswerAll();
  EXPECT_EQ(code, 3u);
  EXPECT_EQ(answer, 42);
  EXPECT_TRUE(req_weak.expired());
  EXPECT_TRUE(rsp_weak.expired());
}

TEST(RpcHandle, FutureSharedCallKeepsRequestAndResponseAlive) {
  DeferredService svc;
  SharedFutureProbe probe(ServiceRef(&svc.base), "json", "svc");
  auto rsp = std::make_shared<int>(0);
  std::weak_ptr<int> rsp_weak = rsp;
  std::weak_ptr<const int> req_weak;
  std::future<Status> future;
  {
    auto req = std::make_shared<const int>(9);
    req_weak = req;
    future = probe.Run(std::move(req), rsp);
  }
  ASSERT_EQ(DeferredService::pending.size(), 1u);
  EXPECT_FALSE(req_weak.expired());

  DeferredService::AnswerAll();
  EXPECT_EQ(future.get().Code(), 3u);
  EXPECT_EQ(*rsp, 10);
  EXPECT_TRUE(req_weak.expired());
  rsp.reset();
  EXPECT_TRUE(rsp_weak.expired());
}

TEST(RpcHandle, SharedCallWithNullRequestOrResponseFailsWithoutCallingProvider) {
  DeferredService svc;
  SharedAsyncProbe async_probe(ServiceRef(&svc.base), "json", "svc");
  SharedFutureProbe future_probe(ServiceRef(&svc.base), "json", "svc");
  uint32_t code = 0;
  async_probe.Run(nullptr, std::make_shared<int>(0), [&code](Status status) { code = status.Code(); });
  EXPECT_EQ(code, static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_UNKNOWN));
  EXPECT_EQ(future_probe.Run(std::make_shared<const int>(0), nullptr).get().Code(),
            static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_UNKNOWN));
  EXPECT_TRUE(DeferredService::pending.empty());
}

// API-054: Status only converts to bool where a condition is expected.
static_assert(!std::is_convertible_v<Status, bool>);
static_assert(std::is_constructible_v<bool, Status>);

TEST(RpcStatus, ExplicitBoolFollowsOk) {
  EXPECT_TRUE(Status(IBMW_RPC_STATUS_OK));
  EXPECT_FALSE(Status(IBMW_RPC_STATUS_OK + 1));
}

}  // namespace
}  // namespace ibmw::rpc
