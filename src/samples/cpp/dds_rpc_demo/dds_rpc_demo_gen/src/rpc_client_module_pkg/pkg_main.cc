// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for dds_rpc_client_app.

#include "pkg/pkg_macro.h"

#include "rpc_client_module/rpc_client_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"RpcClientModule", []() -> ibmw::Module* {
           return new dds_rpc_demo::RpcClientModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
