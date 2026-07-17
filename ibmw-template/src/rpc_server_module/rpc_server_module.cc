// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "rpc_server_module/rpc_server_module.h"

using ibmw::protocols::example::GetBarDataReq;
using ibmw::protocols::example::GetBarDataRsp;
using ibmw::protocols::example::GetFooDataReq;
using ibmw::protocols::example::GetFooDataRsp;

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
          MW_INFO("GetFooData called, req.msg='{}'", req.msg());

          rsp.code(0);
          rsp.msg("echo " + req.msg());

          ibmw::protocols::example::ExampleFoo foo;
          foo.code(42);
          foo.data("sample data");
          rsp.data(foo);

          return ibmw::rpc::Status();  // OK
        });

    // Register GetBarData handler (inline)
    server_handle->GetBarData.ServeInline(
        [this](ibmw::rpc::ContextRef ctx,
               const GetBarDataReq& req,
               GetBarDataRsp& rsp) {
          MW_INFO("GetBarData called, req.msg='{}', req.data.num={}",
                  req.msg(), req.data().num());

          rsp.code(0);
          rsp.msg("echo " + req.msg());

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
