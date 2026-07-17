// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <cstring>

#include "pkg/pkg_macro.h"
#include "benchmark_rpc_client_module/benchmark_rpc_client_module.h"
#include "normal_rpc_async_client_module/normal_rpc_async_client_module.h"
#include "normal_rpc_co_client_module/normal_rpc_co_client_module.h"
#include "normal_rpc_future_client_module/normal_rpc_future_client_module.h"
#include "normal_rpc_sync_client_module/normal_rpc_sync_client_module.h"

using namespace ibmw::examples::cpp::ros2_rpc;

static std::tuple<std::string_view, std::function<ibmw::Module*()>> ibmw_module_register_array[]{
    {"NormalRpcCoClientModule", []() -> ibmw::Module* {
       return new normal_rpc_co_client_module::NormalRpcCoClientModule();
     }},
    {"NormalRpcAsyncClientModule", []() -> ibmw::Module* {
       return new normal_rpc_async_client_module::NormalRpcAsyncClientModule();
     }},
    {"NormalRpcSyncClientModule", []() -> ibmw::Module* {
       return new normal_rpc_sync_client_module::NormalRpcSyncClientModule();
     }},
    {"NormalRpcFutureClientModule", []() -> ibmw::Module* {
       return new normal_rpc_future_client_module::NormalRpcFutureClientModule();
     }},
    {"BenchmarkRpcClientModule", []() -> ibmw::Module* {
       return new benchmark_rpc_client_module::BenchmarkRpcClientModule();
     }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
