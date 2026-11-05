// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/module_base.h"
#include "ros2_rpc_server_module_typesupport.inc.h"

namespace ibmw::context {
class Context;
}

namespace my_project {

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

  IBMW_TS_RPC_SERVER_HANDLER_DECL

 private:
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
  std::shared_ptr<ibmw::context::Context> ctx_ptr_;
};

}  // namespace my_project
