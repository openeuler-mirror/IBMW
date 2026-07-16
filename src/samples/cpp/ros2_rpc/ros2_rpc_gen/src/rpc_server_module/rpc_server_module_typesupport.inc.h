// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>

#include "RosTestRpc.ibmw_rpc.srv.h"

namespace ros2_rpc_demo {

class RpcServerModule;

class _IbmwRos2RpcServerRpcServerModuleRosTestRpcSyncServiceImpl : public example_ros2::srv::RosTestRpcSyncService {
 public:
  explicit _IbmwRos2RpcServerRpcServerModuleRosTestRpcSyncServiceImpl(RpcServerModule* owner) : owner_(owner) {}
  ~_IbmwRos2RpcServerRpcServerModuleRosTestRpcSyncServiceImpl() override = default;

  ibmw::rpc::Status RosTestRpc(
      ibmw::rpc::ContextRef ctx,
      const example_ros2::srv::RosTestRpc_Request& req,
      example_ros2::srv::RosTestRpc_Response& rsp) override;

 private:
  RpcServerModule* owner_;
};

}  // namespace ros2_rpc_demo

#define IBMW_TS_RPC_SERVER_SETUP(core_expr)                                   \
  do {                                                                       \
    core_ = (core_expr);                                                      \
    try {                                                                    \
      service_ptr_ = std::make_shared<_IbmwRos2RpcServerRpcServerModuleRosTestRpcSyncServiceImpl>(this);                   \
      bool ret = core_.GetService().RegisterService(service_ptr_.get());      \
      MW_REQUIRE(ret, "Register ROS2 RPC service failed.");                  \
      MW_INFO("Register service succeeded.");                                \
    } catch (const std::exception& e) {                                     \
      MW_ERROR("Setup failed: {}", e.what());                              \
      return false;                                                           \
    }                                                                        \
  } while (0)

#define IBMW_TS_RPC_SERVER_HANDLER_DECL                                        \
  ibmw::rpc::Status HandleRosTestRpc(                                           \
      ibmw::rpc::ContextRef ctx,                                              \
      const example_ros2::srv::RosTestRpc_Request& req,                                               \
      example_ros2::srv::RosTestRpc_Response& rsp);                                                   \
  std::shared_ptr<_IbmwRos2RpcServerRpcServerModuleRosTestRpcSyncServiceImpl> service_ptr_;

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name) /* defined in inc.cc */
