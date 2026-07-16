// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for JSON RPC demo.
// Registers both JsonRpcServerModule and JsonRpcClientModule.

#include "pkg/pkg_macro.h"

#include "json_rpc_server_module/json_rpc_server_module.h"
#include "json_rpc_client_module/json_rpc_client_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"JsonRpcServerModule", []() -> ibmw::Module* {
           return new my_project::JsonRpcServerModule();
         }},
        {"JsonRpcClientModule", []() -> ibmw::Module* {
           return new my_project::JsonRpcClientModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
