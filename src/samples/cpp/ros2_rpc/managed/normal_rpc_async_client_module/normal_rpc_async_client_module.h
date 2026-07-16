// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <future>
#include <memory>

#include "cpp/module_base.h"

#include "RosTestRpc.ibmw_rpc.srv.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_async_client_module {

class NormalRpcAsyncClientModule : public ibmw::Module {
 public:
  NormalRpcAsyncClientModule() = default;
  ~NormalRpcAsyncClientModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "NormalRpcAsyncClientModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override;

  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  void MainLoopFunc();

 private:
  ibmw::Runtime core_;
  ibmw::executor::ExecutorHandle executor_;

  uint32_t count_ = 0;
  std::atomic_bool run_flag_ = false;
  std::promise<void> stop_sig_;

  double rpc_frq_ = 1.0;
};

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_async_client_module
