// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_rpc_server_module/ros2_rpc_server_module.h"

#include "ros2_rpc_server_module/ros2_rpc_server_module_typesupport.inc.cc"

namespace my_project {

bool Ros2RpcServerModule::Setup(ibmw::Runtime core) {
  core_ = core;
  IBMW_TS_RPC_SERVER_SETUP(core);
  return true;
}

bool Ros2RpcServerModule::Run() {
  MW_INFO("Ros2RpcServerModule::Run succeeded.");
  return true;
}

void Ros2RpcServerModule::Stop() {
  MW_INFO("Ros2RpcServerModule::Stop succeeded.");
}

IBMW_TS_RPC_SERVER_HANDLER_IMPL(Ros2RpcServerModule)

}  // namespace my_project
