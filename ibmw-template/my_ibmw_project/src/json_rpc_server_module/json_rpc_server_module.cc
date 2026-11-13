// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "json_rpc_server_module/json_rpc_server_module.h"

#include "json_rpc_server_module/json_rpc_server_module_typesupport.inc.cc"

namespace my_project {

bool JsonRpcServerModule::Setup(ibmw::Runtime core) {
  core_ = core;
  IBMW_TS_RPC_SERVER_SETUP(core);
  return true;
}

bool JsonRpcServerModule::Run() {
  MW_INFO("JsonRpcServerModule::Run succeeded.");
  return true;
}

void JsonRpcServerModule::Stop() {
  MW_INFO("JsonRpcServerModule::Stop succeeded.");
}

IBMW_TS_RPC_SERVER_HANDLER_IMPL(JsonRpcServerModule)

}  // namespace my_project
