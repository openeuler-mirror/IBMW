// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <memory>

#include "cpp/co/async_scope.h"
#include "cpp/co/task.h"
#include "cpp/module_base.h"

#include "RosTestRpc.ibmw_rpc.srv.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_co_client_module {

class NormalRpcCoClientModule : public ibmw::Module {
 public:
  NormalRpcCoClientModule() = default;
  ~NormalRpcCoClientModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "NormalRpcCoClientModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override;

  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  co::Task<void> MainLoop();

 private:
  ibmw::Runtime core_;
  ibmw::executor::ExecutorHandle executor_;

  co::AsyncScope scope_;
  std::atomic_bool run_flag_ = true;

  double rpc_frq_ = 1.0;
  std::shared_ptr<example_ros2::srv::RosTestRpcCoAccess> access_;
};

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_co_client_module
