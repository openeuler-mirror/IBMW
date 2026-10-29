// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests: blocking / future RPC accessors must tolerate a provider
// that completes the same call more than once.

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace ibmw::rpc
