// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

//
// DDS RPC Client module — calls service methods using coroutines.

#pragma once

#include <memory>

#include "cpp/ibmw_sdk.h"
#include "cpp/co/async_scope.h"
#include "ExampleRpc.ibmw_rpc.dds.h"

namespace my_project {

class RpcClientModule : public ibmw::Module {
 public:
  RpcClientModule() = default;
  ~RpcClientModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "RpcClientModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  ibmw::logger::LoggerRef GetLogger() const { return ctx_ptr_->GetLogger(); }
  ibmw::co::Task<void> RpcLoop();

  std::shared_ptr<ibmw::context::Context> ctx_ptr_;
  ibmw::protocols::example::ExampleServiceCoClient client_;
  ibmw::executor::ExecutorHandle time_executor_;
  ibmw::co::AsyncScope scope_;
};

}  // namespace my_project
