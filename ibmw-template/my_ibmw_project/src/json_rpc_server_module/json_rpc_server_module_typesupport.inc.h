// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>

#include "cpp/service/rpc_handle.h"
#include "cpp/util/json_rpc_helpers.h"
#include "json_custom_types.h"

namespace my_project {

class JsonRpcServerModule;

class _IbmwJsonRpcServerJsonRpcServerModuleComputeServiceImpl : public ibmw::rpc::ServiceBase {
 public:
  explicit _IbmwJsonRpcServerJsonRpcServerModuleComputeServiceImpl(JsonRpcServerModule* owner);

  ~_IbmwJsonRpcServerJsonRpcServerModuleComputeServiceImpl() override = default;

 private:
  JsonRpcServerModule* owner_;
};

}  // namespace my_project

#define IBMW_TS_RPC_SERVER_SETUP(core_expr)                                     do {                                                                           core_ = (core_expr);                                                          service_ = std::make_unique<_IbmwJsonRpcServerJsonRpcServerModuleComputeServiceImpl>(this);                             auto rpc_handle = core_.GetService();                                         bool ret = rpc_handle.RegisterService(service_.get());                        if (!ret) {                                                                    MW_ERROR("Failed to register ComputeService!");                              return false;                                                               }                                                                            MW_INFO("JsonRpcServerModule::Setup succeeded — ComputeService registered.");           } while (0)

#define IBMW_TS_RPC_SERVER_HANDLER_DECL                                          uint32_t HandleCompute(const ComputeRequest& req, ComputeResponse& rsp); \
  std::unique_ptr<ibmw::rpc::ServiceBase> service_;

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name) /* defined in inc.cc */
