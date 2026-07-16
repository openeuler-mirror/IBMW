// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "rpc_client_module/rpc_client_module.h"

#include "cpp/co/ibmw_context.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"

using ibmw::protocols::example::GetFooDataReq;
using ibmw::protocols::example::GetFooDataRsp;
using ibmw::protocols::example::GetBarDataReq;
using ibmw::protocols::example::GetBarDataRsp;

namespace my_project {

bool RpcClientModule::Setup(ibmw::Runtime core) {
  ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
  ctx_ptr_->LetMe();

  try {
    // Create timer executor for coroutine scheduling
    time_executor_ = ctx_ptr_->CreateExecutor("time_schedule_executor");

    // Initialize client
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

ibmw::co::Task<void> RpcClientModule::RpcLoop() {
  try {
    MW_INFO("Start RPC loop.");

    uint32_t counter = 0;
    while (ctx_ptr_->Running()) {
      ctx_ptr_->LetMe();

      // --- Call GetFooData ---
      {
        GetFooDataReq req;
        GetFooDataRsp rsp;
        // TODO: populate req fields

        auto status = co_await client_.GetFooData.Call(req, rsp);
        ctx_ptr_->LetMe();

        if (status.OK()) {
          MW_INFO("GetFooData OK");
        } else {
          MW_WARN("GetFooData FAILED: {}", status.ToString());
        }
      }

      // --- Call GetBarData ---
      {
        GetBarDataReq req;
        GetBarDataRsp rsp;
        // TODO: populate req fields

        auto status = co_await client_.GetBarData.Call(req, rsp);
        ctx_ptr_->LetMe();

        if (status.OK()) {
          MW_INFO("GetBarData OK");
        } else {
          MW_WARN("GetBarData FAILED: {}", status.ToString());
        }
      }

      ++counter;

      // Wait 2 seconds before next round
      co_await ibmw::co::ScheduleAfter(
          ibmw::co::IbmwExecutor(time_executor_),
          std::chrono::seconds(2));
    }

    MW_INFO("Exit RPC loop.");

  } catch (const std::exception& e) {
    MW_ERROR("RpcLoop failed: {}", e.what());
  }

  co_return;
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

}  // namespace my_project
