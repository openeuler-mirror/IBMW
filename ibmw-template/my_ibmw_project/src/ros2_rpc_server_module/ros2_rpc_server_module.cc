// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_rpc_server_module/ros2_rpc_server_module.h"

namespace my_project {

bool Ros2RpcServerModule::Setup(ibmw::Runtime core) {
  core_ = core;

  try {
    // TODO: Create and register your ROS2 RPC service
    // service_ptr_ = std::make_shared<YourServiceImpl>();
    // bool ret = core_.GetService().RegisterService(service_ptr_.get());
    // MW_REQUIRE(ret, "Register ROS2 RPC service failed.");

    MW_INFO("Ros2RpcServerModule::Setup succeeded.");
  } catch (const std::exception& e) {
    MW_ERROR("Setup failed: {}", e.what());
    return false;
  }

  return true;
}

bool Ros2RpcServerModule::Run() {
  MW_INFO("Ros2RpcServerModule::Run succeeded.");
  return true;
}

void Ros2RpcServerModule::Stop() {
  MW_INFO("Ros2RpcServerModule::Stop succeeded.");
}

}  // namespace my_project
