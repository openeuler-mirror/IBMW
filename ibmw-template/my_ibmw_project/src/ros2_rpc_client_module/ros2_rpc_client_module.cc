// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_rpc_client_module/ros2_rpc_client_module.h"

#include "yaml-cpp/yaml.h"

#include "ros2_rpc_client_module/ros2_rpc_client_module_typesupport.inc.cc"

namespace my_project {

bool Ros2RpcClientModule::Setup(ibmw::Runtime core) {
  core_ = core;

  try {
    std::string file_path = std::string(core_.GetConfigurator().GetConfigFilePath());
    if (!file_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(file_path);
      if (cfg_node["Ros2RpcClientModule"] && cfg_node["Ros2RpcClientModule"]["rpc_frq"]) {
        rpc_frq_ = cfg_node["Ros2RpcClientModule"]["rpc_frq"].as<double>();
      }
    }

    auto rpc_handle = core_.GetService();
    MW_CHECK_LOG_THROW(core_.GetLogger(), rpc_handle,
        ibmw::common::util::kLogLevelError, "Get service handle failed.");

    bool ret = example_ros2::srv::RegisterRosTestRpcClientFunc(rpc_handle);
    MW_REQUIRE(ret, "Register ROS2 RPC client failed.");

    access_ = std::make_shared<example_ros2::srv::RosTestRpcSyncAccess>(rpc_handle);

    MW_INFO("Ros2RpcClientModule::Setup succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("Setup failed: {}", e.what());
    return false;
  }

  return true;
}

bool Ros2RpcClientModule::Run() {
  loop_thread_ = std::thread([this]() { RpcLoop(); });

  MW_INFO("Ros2RpcClientModule::Run succeeded.");
  return true;
}

void Ros2RpcClientModule::Stop() {
  run_flag_ = false;
  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }
  MW_INFO("Ros2RpcClientModule::Stop succeeded.");
}

IBMW_TS_RPC_CLIENT_LOOP_IMPL(Ros2RpcClientModule)

}  // namespace my_project
