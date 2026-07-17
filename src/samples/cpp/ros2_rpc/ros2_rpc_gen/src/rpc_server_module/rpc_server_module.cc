// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "rpc_server_module/rpc_server_module.h"

#include "rpc_server_module/rpc_server_module_typesupport.inc.cc"

namespace ros2_rpc_demo {

bool RpcServerModule::Setup(ibmw::Runtime core) {
  core_ = core;
  IBMW_TS_RPC_SERVER_SETUP(core);
  return true;
}

bool RpcServerModule::Run() {
  MW_INFO("RpcServerModule::Run succeeded.");
  return true;
}

void RpcServerModule::Stop() {
  MW_INFO("RpcServerModule::Stop succeeded.");
}

IBMW_TS_RPC_SERVER_HANDLER_IMPL(RpcServerModule)

}  // namespace ros2_rpc_demo
