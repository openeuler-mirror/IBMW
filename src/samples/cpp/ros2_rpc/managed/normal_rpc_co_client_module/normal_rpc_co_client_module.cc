// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "normal_rpc_co_client_module/normal_rpc_co_client_module.h"
#include "cpp/co/ibmw_context.h"
#include "cpp/co/inline_executor.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_co_client_module {

bool NormalRpcCoClientModule::Setup(ibmw::Runtime core) {
  core_ = core;

  try {
    // Read cfg
    std::string file_path = std::string(core_.GetConfigurator().GetConfigFilePath());
    if (!file_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(file_path);
      rpc_frq_ = cfg_node["rpc_frq"].as<double>();
    }

    // Get executor handle
    executor_ = core_.GetExecutor().GetExecutor("work_thread_pool");
    MW_REQUIRE(executor_ && executor_.SupportsTimedDispatch(),
                            "Get executor 'work_thread_pool' failed.");

    // Get service handle
    auto rpc_handle = core_.GetService();
    MW_REQUIRE(rpc_handle, "Get service handle failed.");

    // Register rpc client
    bool ret = example_ros2::srv::RegisterRosTestRpcClientFunc(rpc_handle);
    MW_REQUIRE(ret, "Register client failed.");

    // Create rpc access
    access_ = std::make_shared<example_ros2::srv::RosTestRpcCoAccess>(rpc_handle);

    // Register filter
    access_->RegisterFilter([this](ibmw::rpc::ContextRef ctx,
                                  const void* req_ptr, void* rsp_ptr,
                                  const ibmw::rpc::CoRpcHandle& next)
                               -> co::Task<ibmw::rpc::Status> {
      // timecost count
      auto begin_time = std::chrono::steady_clock::now();
      const auto& status = co_await next(ctx, req_ptr, rsp_ptr);
      auto end_time = std::chrono::steady_clock::now();

      MW_INFO("Client rpc time cost {} us",
                 std::chrono::duration_cast<std::chrono::microseconds>(end_time - begin_time).count());

      co_return status;
    });

  } catch (const std::exception& e) {
    MW_ERROR("Init failed, {}", e.what());
    return false;
  }

  MW_INFO("Setup succeeded.");

  return true;
}

bool NormalRpcCoClientModule::Run() {
  try {
    scope_.spawn(co::On(co::InlineExecutor(), MainLoop()));
  } catch (const std::exception& e) {
    MW_ERROR("Start failed, {}", e.what());
    return false;
  }

  MW_INFO("Run succeeded.");
  return true;
}

void NormalRpcCoClientModule::Stop() {
  try {
    run_flag_ = false;
    co::SyncWait(scope_.complete());
  } catch (const std::exception& e) {
    MW_ERROR("Shutdown failed, {}", e.what());
    return;
  }

  MW_INFO("Stop succeeded.");
}

// Main loop
co::Task<void> NormalRpcCoClientModule::MainLoop() {
  try {
    MW_INFO("Start MainLoop.");

    co::IbmwExecutor work_thread_pool_executor(executor_);

    co_await co::Schedule(work_thread_pool_executor);

    uint32_t count = 0;
    while (run_flag_) {
      co_await co::ScheduleAfter(
          work_thread_pool_executor,
          std::chrono::milliseconds(static_cast<uint32_t>(1000 / rpc_frq_)));
      count++;
      MW_INFO("Loop count : {} -------------------------", count);

      // call rpc
      example_ros2::srv::RosTestRpc_Request req;
      example_ros2::srv::RosTestRpc_Response rsp;
      req.data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

      MW_INFO("start new service call. req:\n{}", example_ros2::srv::to_yaml(req));

      auto ctx_ptr = access_->NewContextSharedPtr();
      ctx_ptr->SetTimeout(std::chrono::seconds(3));

      auto status = co_await access_->RosTestRpc(ctx_ptr, req, rsp);

      MW_INFO("Get rpc ret, status: {}. rsp:\n{}",
                 status.ToString(), example_ros2::srv::to_yaml(rsp));

      MW_CHECK_WARN(status, "Call GetFooData failed, status: {}", status.ToString());
    }

    MW_INFO("Exit MainLoop.");
  } catch (const std::exception& e) {
    MW_ERROR("Exit MainLoop with exception, {}", e.what());
  }

  co_return;
}

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_co_client_module
