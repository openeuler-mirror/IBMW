// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "rpc_server_on_executor_module/rpc_server_on_executor_module.h"
#include "cpp/co/ibmw_context.h"
#include "cpp/co/async_scope.h"
#include "cpp/co/inline_executor.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"
#include "service/rpc_context.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::context::RpcServerOnExecutorModule {

bool RpcServerOnExecutorModule::Setup(ibmw::Runtime core) {
  ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
  ctx_ptr_->LetMe();
  try {
    auto cfg_path = ctx_ptr_->GetConfigFilePath();
    if (!cfg_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(cfg_path));
    }

    work_executor_ = ctx_ptr_->CreateExecutor("work_thread_pool");

    auto server_handle = ctx_ptr_->CreateServer<ibmw::protocols::example::ExampleServiceCoServer>();

    server_handle->GetBarData.ServeOn(work_executor_, [this](ibmw::rpc::ContextRef ctx, const ibmw::protocols::example::GetBarDataReq& req, ibmw::protocols::example::GetBarDataRsp& rsp) {
      MW_INFO("RpcServerOnExecutorModule handle GetBarData service call. context: {}, req msg: {}",
                 ctx.ToString(), req.msg());
      rsp.msg("echo " + req.msg());
      return ibmw::rpc::Status();
    });

    server_handle->GetFooData.ServeOn(work_executor_, [this](ibmw::rpc::ContextRef ctx, const ibmw::protocols::example::GetFooDataReq& req, ibmw::protocols::example::GetFooDataRsp& rsp) {
      MW_INFO("RpcServerOnExecutorModule handle GetFooData service call. context: {}, req msg: {}",
                 ctx.ToString(), req.msg());
      rsp.msg("echo " + req.msg());
      return ibmw::rpc::Status();
    });

  } catch (const std::exception& e) {
    MW_ERROR("RpcServerOnExecutorModule init failed: {}", e.what());
    return false;
  }

  return true;
}

bool RpcServerOnExecutorModule::Run() {
  ctx_ptr_->LetMe();
  return true;
}

void RpcServerOnExecutorModule::Stop() {
  try {
    ctx_ptr_->StopRunning();

    MW_INFO("RpcServerOnExecutorModule shutdown succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("RpcServerOnExecutorModule shutdown failed: {}", e.what());
  }
}

}  // namespace ibmw::examples::cpp::context::RpcServerOnExecutorModule
