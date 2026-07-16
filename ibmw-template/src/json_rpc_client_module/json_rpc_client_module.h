// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// JSON RPC demo — Client module.
// Calls "Compute" RPC method using SyncAccessBase pattern.
// Uses JSON type support (no IDL/code-gen).

#pragma once

#include <atomic>

#include "cpp/module_base.h"
#include "cpp/service/rpc_handle.h"

namespace my_project {

class JsonRpcClientModule : public ibmw::Module {
 public:
  JsonRpcClientModule() = default;
  ~JsonRpcClientModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "JsonRpcClientModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void RpcLoop();
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
  ibmw::rpc::ServiceRef service_ref_;
  std::atomic<bool> running_{false};
};

}  // namespace my_project
