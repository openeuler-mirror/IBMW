// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>

#include "cpp/module_base.h"
#include "normal_rpc_async_server_module/service.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_async_server_module {

class NormalRpcAsyncServerModule : public ibmw::Module {
 public:
  NormalRpcAsyncServerModule() = default;
  ~NormalRpcAsyncServerModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "NormalRpcAsyncServerModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override;

  void Stop() override;

 private:
  ibmw::Runtime core_;
  std::shared_ptr<RosTestRpcAsyncServiceImpl> service_ptr_;
};

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_async_server_module
