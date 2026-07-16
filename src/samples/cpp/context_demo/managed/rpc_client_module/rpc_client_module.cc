// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "rpc_client_module/rpc_client_module.h"
#include "cpp/co/ibmw_context.h"
#include "cpp/co/async_scope.h"
#include "cpp/co/inline_executor.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"
#include "service/rpc_context.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::context::RpcClientModule {

bool RpcClientModule::Setup(ibmw::Runtime core) {
  ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
  ctx_ptr_->LetMe();
  try {
    auto cfg_path = ctx_ptr_->GetConfigFilePath();
    if (!cfg_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(cfg_path));
      rpc_frq_ = cfg_node["rpc_frq"].as<double>();
    }

    time_executor_ = ctx_ptr_->CreateExecutor("time_schedule_executor");

    client_.Init(ctx_ptr_);

  } catch (const std::exception& e) {
    MW_ERROR("RpcClientModule init failed: {}", e.what());
    return false;
  }

  return true;
}

bool RpcClientModule::Run() {
  try {
    auto executor = ibmw::co::IbmwExecutor(time_executor_);
    scope_.spawn(ibmw::co::On(executor, RpcTask()));

    MW_INFO("RpcClientModule start succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("RpcClientModule start failed: {}", e.what());
    return false;
  }
  return true;
}

ibmw::co::Task<void> RpcClientModule::RpcTask() {
  try {
    MW_INFO("Start RPC loop.");

    while (ctx_ptr_->Running()) {
      ctx_ptr_->LetMe();  // Re-establish thread-local context after each co_await resumption
      ibmw::protocols::example::GetFooDataReq req;
      ibmw::protocols::example::GetFooDataRsp rsp;
      req.msg("hello world");
      auto status = co_await client_.GetFooData.Call(req, rsp);
      if (status.OK()) {
        MW_INFO("RPC call succeeded, response: {}", rsp.msg());
      } else {
        MW_WARN("RPC call failed, status: {}", status.ToString());
      }

      co_await ibmw::co::ScheduleAfter(ibmw::co::IbmwExecutor(time_executor_), std::chrono::milliseconds(static_cast<uint32_t>(1000 / rpc_frq_)));
    }

    MW_INFO("Exit RPC loop.");
  } catch (const std::exception& e) {
    MW_ERROR("RpcTask failed: {}", e.what());
  }

  co_return;
}

void RpcClientModule::Stop() {
  try {
    ctx_ptr_->StopRunning();
    ibmw::co::SyncWait(scope_.complete());

    MW_INFO("RpcClientModule shutdown succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("RpcClientModule shutdown failed: {}", e.what());
  }
}

}  // namespace ibmw::examples::cpp::context::RpcClientModule
