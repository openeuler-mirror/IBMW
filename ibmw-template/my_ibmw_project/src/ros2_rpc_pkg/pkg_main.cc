// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for ros2_rpc_app.

#include "pkg/pkg_macro.h"

#include "ros2_rpc_server_module/ros2_rpc_server_module.h"
#include "ros2_rpc_client_module/ros2_rpc_client_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"Ros2RpcServerModule", []() -> ibmw::Module* {
           return new my_project::Ros2RpcServerModule();
         }},
        {"Ros2RpcClientModule", []() -> ibmw::Module* {
           return new my_project::Ros2RpcClientModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
