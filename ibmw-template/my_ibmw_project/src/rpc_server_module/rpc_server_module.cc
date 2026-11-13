// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "rpc_server_module/rpc_server_module.h"

#include "rpc_server_module/rpc_server_module_typesupport.inc.cc"

namespace my_project {

bool RpcServerModule::Setup(ibmw::Runtime core) {
  core_ = core;
  IBMW_TS_RPC_SERVER_SETUP(core);
  return true;
}

bool RpcServerModule::Run() {
  try {
    if (ctx_ptr_) {
      ctx_ptr_->LetMe();
    }
    MW_INFO("RpcServerModule::Run succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("RpcServerModule::Run failed: {}", e.what());
    return false;
  }
  return true;
}

void RpcServerModule::Stop() {
  try {
    if (ctx_ptr_) {
      ctx_ptr_->StopRunning();
    }
    MW_INFO("RpcServerModule::Stop succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("RpcServerModule::Stop failed: {}", e.what());
  }
}

IBMW_TS_RPC_SERVER_HANDLER_IMPL(RpcServerModule)

}  // namespace my_project
