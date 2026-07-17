// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_rpc_client_module/ros2_rpc_client_module.h"

#include "cpp/co/ibmw_context.h"
#include "cpp/co/inline_executor.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"

namespace my_project {

bool Ros2RpcClientModule::Setup(ibmw::Runtime core) {
  core_ = core;

  try {
    // Get executor handle
    executor_ = core_.GetExecutor().GetExecutor("work_thread_pool");
    MW_REQUIRE(executor_ && executor_.SupportsTimedDispatch(),
               "Get executor 'work_thread_pool' failed.");

    // Get service handle
    auto rpc_handle = core_.GetService();
    MW_REQUIRE(rpc_handle, "Get service handle failed.");

    // Register ROS2 RPC client
    bool ret = example_ros2::srv::RegisterRosTestRpcClientFunc(rpc_handle);
    MW_REQUIRE(ret, "Register ROS2 RPC client failed.");

    // Create access object
    access_ = std::make_shared<example_ros2::srv::RosTestRpcCoAccess>(rpc_handle);

    MW_INFO("Ros2RpcClientModule::Setup succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("Setup failed: {}", e.what());
    return false;
  }

  return true;
}

bool Ros2RpcClientModule::Run() {
  try {
    scope_.spawn(ibmw::co::On(ibmw::co::InlineExecutor(), MainLoop()));
  } catch (const std::exception& e) {
    MW_ERROR("Run failed: {}", e.what());
    return false;
  }

  MW_INFO("Ros2RpcClientModule::Run succeeded.");
  return true;
}

void Ros2RpcClientModule::Stop() {
  try {
    run_flag_ = false;
    ibmw::co::SyncWait(scope_.complete());
  } catch (const std::exception& e) {
    MW_ERROR("Stop failed: {}", e.what());
    return;
  }

  MW_INFO("Ros2RpcClientModule::Stop succeeded.");
}

ibmw::co::Task<void> Ros2RpcClientModule::MainLoop() {
  try {
    MW_INFO("Start ROS2 RPC MainLoop.");

    ibmw::co::IbmwExecutor work_executor(executor_);
    co_await ibmw::co::Schedule(work_executor);

    uint32_t count = 0;
    while (run_flag_) {
      co_await ibmw::co::ScheduleAfter(
          work_executor, std::chrono::seconds(1));
      count++;

      MW_INFO("RPC call #{} -------------------------", count);

      // Build request
      example_ros2::srv::RosTestRpc_Request req;
      example_ros2::srv::RosTestRpc_Response rsp;
      req.data = {0x01, 0x02, 0x03, 0x04, 0x05};

      auto ctx_ptr = access_->NewContextSharedPtr();
      ctx_ptr->SetTimeout(std::chrono::seconds(3));

      auto status = co_await access_->RosTestRpc(ctx_ptr, req, rsp);

      if (status.OK()) {
        MW_INFO("RPC OK: code={}", rsp.code);
      } else {
        MW_WARN("RPC failed: {}", status.ToString());
      }
    }

    MW_INFO("Exit ROS2 RPC MainLoop.");
  } catch (const std::exception& e) {
    MW_ERROR("MainLoop exception: {}", e.what());
  }

  co_return;
}

}  // namespace my_project
