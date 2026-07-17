// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// ROS2 RPC demo — Server module.
// Implements example_ros2::srv::RosTestRpc using ROS2 generated service types.

#pragma once

#include <memory>

#include "cpp/module_base.h"
#include "RosTestRpc.ibmw_rpc.srv.h"

namespace my_project {

class Ros2RpcServiceImpl : public example_ros2::srv::RosTestRpcCoService {
 public:
  Ros2RpcServiceImpl() = default;
  ~Ros2RpcServiceImpl() override = default;

  void SetLogger(ibmw::logger::LoggerRef logger) { logger_ = logger; }

  ibmw::co::Task<ibmw::rpc::Status> RosTestRpc(
      ibmw::rpc::ContextRef ctx,
      const example_ros2::srv::RosTestRpc_Request& req,
      example_ros2::srv::RosTestRpc_Response& rsp) override;

 private:
  ibmw::logger::LoggerRef logger_;
};

class Ros2RpcServerModule : public ibmw::Module {
 public:
  Ros2RpcServerModule() = default;
  ~Ros2RpcServerModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "Ros2RpcServerModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
  std::shared_ptr<Ros2RpcServiceImpl> service_ptr_;
};

}  // namespace my_project
