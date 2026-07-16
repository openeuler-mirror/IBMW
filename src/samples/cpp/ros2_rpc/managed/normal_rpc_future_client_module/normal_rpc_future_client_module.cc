// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "normal_rpc_future_client_module/normal_rpc_future_client_module.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_future_client_module {

bool NormalRpcFutureClientModule::Setup(ibmw::Runtime core) {
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

  } catch (const std::exception& e) {
    MW_ERROR("Init failed, {}", e.what());
    return false;
  }

  MW_INFO("Setup succeeded.");

  return true;
}

bool NormalRpcFutureClientModule::Run() {
  try {
    run_flag_ = true;
    executor_.Dispatch(std::bind(&NormalRpcFutureClientModule::MainLoop, this));
  } catch (const std::exception& e) {
    MW_ERROR("Start failed, {}", e.what());
    return false;
  }

  MW_INFO("Run succeeded.");
  return true;
}

void NormalRpcFutureClientModule::Stop() {
  try {
    if (run_flag_) {
      run_flag_ = false;
      stop_sig_.get_future().wait();
    }
  } catch (const std::exception& e) {
    MW_ERROR("Shutdown failed, {}", e.what());
    return;
  }

  MW_INFO("Stop succeeded.");
}

// Main loop
void NormalRpcFutureClientModule::MainLoop() {
  try {
    MW_INFO("Start MainLoop.");

    example_ros2::srv::RosTestRpcFutureAccess access(core_.GetService());

    uint32_t count = 0;
    while (run_flag_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<uint32_t>(1000 / rpc_frq_)));

      count++;
      MW_INFO("Loop count : {} -------------------------", count);

      // call rpc
      example_ros2::srv::RosTestRpc_Request req;
      example_ros2::srv::RosTestRpc_Response rsp;
      req.data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

      auto ctx_ptr = access.NewContextSharedPtr();
      ctx_ptr->SetTimeout(std::chrono::seconds(3));

      MW_INFO("start new service call. req:\n{}", example_ros2::srv::to_yaml(req));

      auto status_future = access.RosTestRpc(ctx_ptr, req, rsp);
      auto status = status_future.get();

      if (status.OK()) {
        MW_INFO("Client get rpc ret, status: {}, rsp: {}", status.ToString(),
                   example_ros2::srv::to_yaml(rsp));
      } else {
        MW_WARN("Client get rpc error ret, status: {}", status.ToString());
      }
    }

    MW_INFO("Exit MainLoop.");
  } catch (const std::exception& e) {
    MW_ERROR("Exit MainLoop with exception, {}", e.what());
  }

  stop_sig_.set_value();
}

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_future_client_module
