// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests: blocking / future RPC accessors must tolerate a provider
// that completes the same call more than once.

#include <gtest/gtest.h>

#include <deque>

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

TEST(RpcHandle, CoCallOffExecutorFailsWithoutThrowing) {
  Status status;
  EXPECT_NO_THROW(status = RunCo(CoService::Mode::kAnswer, nullptr));
  EXPECT_EQ(status.Code(), static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_INVALID_CONTEXT));
}

}  // namespace
}  // namespace ibmw::rpc
