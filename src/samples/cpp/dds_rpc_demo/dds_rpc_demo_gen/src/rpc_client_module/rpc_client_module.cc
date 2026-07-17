// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "rpc_client_module/rpc_client_module.h"

#include "cpp/co/ibmw_context.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"

#include "rpc_client_module/rpc_client_module_typesupport.inc.cc"

namespace dds_rpc_demo {

bool RpcClientModule::Setup(ibmw::Runtime core) {
  ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
  ctx_ptr_->LetMe();

  try {
    time_executor_ = ctx_ptr_->CreateExecutor("time_schedule_executor");
    client_.Init(ctx_ptr_);
    MW_INFO("RpcClientModule::Setup succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("RpcClientModule::Setup failed: {}", e.what());
    return false;
  }

  return true;
}

bool RpcClientModule::Run() {
  try {
    auto executor = ibmw::co::IbmwExecutor(time_executor_);
    scope_.spawn(ibmw::co::On(executor, RpcLoop()));
    MW_INFO("RpcClientModule::Run succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("RpcClientModule::Run failed: {}", e.what());
    return false;
  }
  return true;
}

void RpcClientModule::Stop() {
  try {
    ctx_ptr_->StopRunning();
    ibmw::co::SyncWait(scope_.complete());
    MW_INFO("RpcClientModule::Stop succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("RpcClientModule::Stop failed: {}", e.what());
  }
}

IBMW_TS_RPC_CLIENT_LOOP_IMPL(RpcClientModule)

}  // namespace dds_rpc_demo
