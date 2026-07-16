// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

//
// ROS2 RPC demo — Client module.
// NOTE: This is a skeleton. ROS2 RPC requires external service type packages.

#pragma once

#include <atomic>
#include <memory>

#include "cpp/co/async_scope.h"
#include "cpp/co/task.h"
#include "cpp/module_base.h"
// TODO: #include your ROS2 RPC generated header, e.g.:
// #include "YourService.ibmw_rpc.srv.h"

namespace my_project {

class Ros2RpcClientModule : public ibmw::Module {
 public:
  Ros2RpcClientModule() = default;
  ~Ros2RpcClientModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "Ros2RpcClientModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::co::Task<void> MainLoop();

  ibmw::Runtime core_;
  ibmw::executor::ExecutorHandle executor_;

  ibmw::co::AsyncScope scope_;
  std::atomic_bool run_flag_{true};

  // TODO: std::shared_ptr<your_package::srv::YourServiceCoAccess> access_;
};

}  // namespace my_project
