// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <cstring>

#include "pkg/pkg_macro.h"
#include "normal_rpc_async_client_module/normal_rpc_async_client_module.h"
#include "normal_rpc_async_server_module/normal_rpc_async_server_module.h"
#include "normal_rpc_co_client_module/normal_rpc_co_client_module.h"
#include "normal_rpc_co_server_module/normal_rpc_co_server_module.h"
#include "normal_rpc_future_client_module/normal_rpc_future_client_module.h"
#include "normal_rpc_sync_client_module/normal_rpc_sync_client_module.h"
#include "normal_rpc_sync_server_module/normal_rpc_sync_server_module.h"

using namespace ibmw::examples::cpp::ros2_rpc;

static std::tuple<std::string_view, std::function<ibmw::Module*()>> ibmw_module_register_array[]{
    {"NormalRpcCoClientModule", []() -> ibmw::Module* {
       return new normal_rpc_co_client_module::NormalRpcCoClientModule();
     }},
    {"NormalRpcCoServerModule", []() -> ibmw::Module* {
       return new normal_rpc_co_server_module::NormalRpcCoServerModule();
     }},
    {"NormalRpcAsyncClientModule", []() -> ibmw::Module* {
       return new normal_rpc_async_client_module::NormalRpcAsyncClientModule();
     }},
    {"NormalRpcAsyncServerModule", []() -> ibmw::Module* {
       return new normal_rpc_async_server_module::NormalRpcAsyncServerModule();
     }},
    {"NormalRpcSyncClientModule", []() -> ibmw::Module* {
       return new normal_rpc_sync_client_module::NormalRpcSyncClientModule();
     }},
    {"NormalRpcSyncServerModule", []() -> ibmw::Module* {
       return new normal_rpc_sync_server_module::NormalRpcSyncServerModule();
     }},
    {"NormalRpcFutureClientModule", []() -> ibmw::Module* {
       return new normal_rpc_future_client_module::NormalRpcFutureClientModule();
     }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
