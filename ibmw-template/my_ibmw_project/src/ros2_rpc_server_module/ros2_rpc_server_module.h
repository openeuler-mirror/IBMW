// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

//
// ROS2 RPC demo — Server module.
// NOTE: This is a skeleton. ROS2 RPC requires external service type packages.

#pragma once

#include <memory>

#include "cpp/module_base.h"
// TODO: #include your ROS2 RPC generated header, e.g.:
// #include "YourService.ibmw_rpc.srv.h"

namespace my_project {

// TODO: Implement your ROS2 RPC service class deriving from
//       your_package::srv::YourServiceCoService

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
  // TODO: std::shared_ptr<YourServiceImpl> service_ptr_;
};

}  // namespace my_project
