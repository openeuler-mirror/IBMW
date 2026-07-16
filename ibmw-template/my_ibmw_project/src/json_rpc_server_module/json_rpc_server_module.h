// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

//
// JSON RPC demo — Server module.
// Uses ibmw::rpc::ServiceBase + JSON type support (no IDL/code-gen).

#pragma once

#include <memory>

#include "cpp/module_base.h"
#include "cpp/service/rpc_handle.h"

namespace my_project {

class JsonRpcServerModule : public ibmw::Module {
 public:
  JsonRpcServerModule() = default;
  ~JsonRpcServerModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "JsonRpcServerModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
  // Use base class pointer — service impl defined in .cc
  std::unique_ptr<ibmw::rpc::ServiceBase> service_;
};

}  // namespace my_project
