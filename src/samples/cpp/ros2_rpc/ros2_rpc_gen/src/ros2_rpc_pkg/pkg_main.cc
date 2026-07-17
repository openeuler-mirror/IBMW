// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for ros2_rpc_app.

#include "pkg/pkg_macro.h"

#include "rpc_server_module/rpc_server_module.h"
#include "rpc_client_module/rpc_client_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"RpcServerModule", []() -> ibmw::Module* {
           return new ros2_rpc_demo::RpcServerModule();
         }},
        {"RpcClientModule", []() -> ibmw::Module* {
           return new ros2_rpc_demo::RpcClientModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
