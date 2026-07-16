// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// RPC Server module — handles GetFooData and GetBarData using ServeInline.
// Uses the pre-built ExampleService RPC generated code shipped with IBMW.

#pragma once

#include <memory>

#include "cpp/ibmw_sdk.h"
#include "ExampleRpc.ibmw_rpc.dds.h"

namespace my_project {

class RpcServerModule : public ibmw::Module {
 public:
  RpcServerModule() = default;
  ~RpcServerModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "RpcServerModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  ibmw::logger::LoggerRef GetLogger() const { return ctx_ptr_->GetLogger(); }

  std::shared_ptr<ibmw::context::Context> ctx_ptr_;
};

}  // namespace my_project
