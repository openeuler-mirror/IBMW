// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/module_base.h"
#include "rpc_client_module_typesupport.inc.h"

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

  IBMW_TS_RPC_CLIENT_LOOP_DECL
  ibmw::Runtime core_;
};

}  // namespace my_project
