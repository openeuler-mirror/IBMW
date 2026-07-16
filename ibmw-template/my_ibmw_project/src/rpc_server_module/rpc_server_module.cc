// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "rpc_server_module/rpc_server_module.h"

using ibmw::protocols::example::GetFooDataReq;
using ibmw::protocols::example::GetFooDataRsp;
using ibmw::protocols::example::GetBarDataReq;
using ibmw::protocols::example::GetBarDataRsp;

namespace my_project {

bool RpcServerModule::Setup(ibmw::Runtime core) {
  ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
  ctx_ptr_->LetMe();

  try {
    // Create the server resource holder for ExampleService
    auto server_handle =
        ctx_ptr_->CreateServer<ibmw::protocols::example::ExampleServiceCoServer>();

    // Register GetFooData handler (inline — runs on the caller's thread)
    server_handle->GetFooData.ServeInline(
        [this](ibmw::rpc::ContextRef ctx,
               const GetFooDataReq& req,
               GetFooDataRsp& rsp) {
          MW_INFO("GetFooData called");
          // TODO: implement handler logic
          return ibmw::rpc::Status();  // OK
        });

    // Register GetBarData handler (inline — runs on the caller's thread)
    server_handle->GetBarData.ServeInline(
        [this](ibmw::rpc::ContextRef ctx,
               const GetBarDataReq& req,
               GetBarDataRsp& rsp) {
          MW_INFO("GetBarData called");
          // TODO: implement handler logic
          return ibmw::rpc::Status();  // OK
        });

    MW_INFO("RpcServerModule::Setup succeeded.");

  } catch (const std::exception& e) {
    MW_ERROR("RpcServerModule::Setup failed: {}", e.what());
    return false;
  }

  return true;
}

bool RpcServerModule::Run() {
  ctx_ptr_->LetMe();
  MW_INFO("RpcServerModule::Run succeeded.");
  return true;
}

void RpcServerModule::Stop() {
  try {
    ctx_ptr_->StopRunning();
    MW_INFO("RpcServerModule::Stop succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("RpcServerModule::Stop failed: {}", e.what());
  }
}

}  // namespace my_project
