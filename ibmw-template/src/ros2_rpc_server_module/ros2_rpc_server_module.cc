// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_rpc_server_module/ros2_rpc_server_module.h"

namespace my_project {

ibmw::co::Task<ibmw::rpc::Status> Ros2RpcServiceImpl::RosTestRpc(
    ibmw::rpc::ContextRef ctx,
    const example_ros2::srv::RosTestRpc_Request& req,
    example_ros2::srv::RosTestRpc_Response& rsp) {
  // Echo request data size as response code
  rsp.code = static_cast<int64_t>(req.data.size());

  MW_LOG(logger_, ibmw::common::util::kLogLevelInfo,
         "ROS2 RPC Server: received {} bytes, responding with code={}",
         req.data.size(), rsp.code);

  co_return ibmw::rpc::Status();
}

bool Ros2RpcServerModule::Setup(ibmw::Runtime core) {
  core_ = core;

  try {
    service_ptr_ = std::make_shared<Ros2RpcServiceImpl>();
    service_ptr_->SetLogger(core_.GetLogger());

    bool ret = core_.GetService().RegisterService(service_ptr_.get());
    MW_REQUIRE(ret, "Register ROS2 RPC service failed.");

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
