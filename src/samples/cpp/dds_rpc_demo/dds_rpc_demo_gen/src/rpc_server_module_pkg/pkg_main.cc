// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for dds_rpc_server_app.

#include "pkg/pkg_macro.h"

#include "rpc_server_module/rpc_server_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"RpcServerModule", []() -> ibmw::Module* {
           return new dds_rpc_demo::RpcServerModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
