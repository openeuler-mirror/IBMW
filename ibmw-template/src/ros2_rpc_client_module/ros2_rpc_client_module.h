// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// ROS2 RPC demo — Client module.
// Calls example_ros2::srv::RosTestRpc using coroutine-based access.

#pragma once

#include <atomic>
#include <memory>

#include "cpp/co/async_scope.h"
#include "cpp/co/task.h"
#include "cpp/module_base.h"
#include "RosTestRpc.ibmw_rpc.srv.h"

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

  std::shared_ptr<example_ros2::srv::RosTestRpcCoAccess> access_;
};

}  // namespace my_project
