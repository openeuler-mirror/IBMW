// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "rpc_client_module/rpc_client_module.h"

#include "yaml-cpp/yaml.h"

#include "rpc_client_module/rpc_client_module_typesupport.inc.cc"

namespace ros2_rpc_demo {

bool RpcClientModule::Setup(ibmw::Runtime core) {
  core_ = core;

  try {
    std::string file_path = std::string(core_.GetConfigurator().GetConfigFilePath());
    if (!file_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(file_path);
      if (cfg_node["RpcClientModule"] && cfg_node["RpcClientModule"]["rpc_frq"]) {
        rpc_frq_ = cfg_node["RpcClientModule"]["rpc_frq"].as<double>();
      }
    }

    auto rpc_handle = core_.GetService();
    MW_CHECK_LOG_THROW(core_.GetLogger(), rpc_handle,
        ibmw::common::util::kLogLevelError, "Get service handle failed.");

    bool ret = example_ros2::srv::RegisterRosTestRpcClientFunc(rpc_handle);
    MW_REQUIRE(ret, "Register ROS2 RPC client failed.");

    access_ = std::make_shared<example_ros2::srv::RosTestRpcSyncAccess>(rpc_handle);

    MW_INFO("RpcClientModule::Setup succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("Setup failed: {}", e.what());
    return false;
  }

  return true;
}

bool RpcClientModule::Run() {
  loop_thread_ = std::thread([this]() { RpcLoop(); });

  MW_INFO("RpcClientModule::Run succeeded.");
  return true;
}

void RpcClientModule::Stop() {
  run_flag_ = false;
  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }
  MW_INFO("RpcClientModule::Stop succeeded.");
}

IBMW_TS_RPC_CLIENT_LOOP_IMPL(RpcClientModule)

}  // namespace ros2_rpc_demo
