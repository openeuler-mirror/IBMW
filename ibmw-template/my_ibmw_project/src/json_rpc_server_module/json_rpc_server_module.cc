// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "json_rpc_server_module/json_rpc_server_module.h"

#include "cpp/util/json_rpc_helpers.h"
#include "json_custom_types.h"

namespace my_project {

// ---------------------------------------------------------------------------
// ComputeServiceImpl — uses ibmw::json::rpc::RegisterServiceFunc for
// type-safe handler registration.
// ---------------------------------------------------------------------------

class ComputeServiceImpl : public ibmw::rpc::ServiceBase {
 public:
  ComputeServiceImpl()
      : ibmw::rpc::ServiceBase("json", "my_project.ComputeService") {
    // Type-safe registration for Compute
    ibmw::json::rpc::RegisterServiceFunc<ComputeRequest, ComputeResponse>(
        *this, "Compute",
        [this](const ComputeRequest& req, ComputeResponse& rsp) -> uint32_t {
          return HandleCompute(req, rsp);
        });
  }

  ~ComputeServiceImpl() override = default;

 private:
  uint32_t HandleCompute(const ComputeRequest& req, ComputeResponse& rsp);
};

uint32_t ComputeServiceImpl::HandleCompute(const ComputeRequest& req, ComputeResponse& rsp) {
  // TODO: implement handler logic
  return 0;
}

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

bool JsonRpcServerModule::Setup(ibmw::Runtime core) {
  core_ = core;

  service_ = std::make_unique<ComputeServiceImpl>();

  // Register the service on the RPC handle
  auto rpc_handle = core_.GetService();
  bool ret = rpc_handle.RegisterService(service_.get());
  if (!ret) {
    MW_ERROR("Failed to register ComputeService!");
    return false;
  }

  MW_INFO("JsonRpcServerModule::Setup succeeded — ComputeService registered.");
  return true;
}

bool JsonRpcServerModule::Run() {
  MW_INFO("JsonRpcServerModule::Run — server ready.");
  return true;
}

void JsonRpcServerModule::Stop() {
  MW_INFO("JsonRpcServerModule::Stop.");
}

}  // namespace my_project
