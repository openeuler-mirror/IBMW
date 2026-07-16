// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string>

#include "cpp/ibmw_sdk.h"
#include "cpp/co/async_scope.h"

#include "ExampleRpc.ibmw_rpc.dds.h"

namespace ibmw::examples::cpp::context::RpcServerOnExecutorModule {

class RpcServerOnExecutorModule : public ibmw::Module {
 public:
  RpcServerOnExecutorModule() = default;
  ~RpcServerOnExecutorModule() override = default;

  ModuleDescriptor Info() const override { return ModuleDescriptor{.module_name = "RpcServerOnExecutorModule"}; }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  ibmw::logger::LoggerRef GetLogger() const { return ctx_ptr_->GetLogger(); }

 private:
  std::shared_ptr<ibmw::context::Context> ctx_ptr_;
  ibmw::executor::ExecutorHandle work_executor_;

  std::string service_name_;
  double rpc_frq_;
};

}  // namespace ibmw::examples::cpp::context::RpcServerOnExecutorModule
