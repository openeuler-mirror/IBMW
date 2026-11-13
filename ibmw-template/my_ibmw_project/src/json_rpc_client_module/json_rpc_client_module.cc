// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "json_rpc_client_module/json_rpc_client_module.h"

#include "json_rpc_client_module/json_rpc_client_module_typesupport.inc.cc"

namespace my_project {

bool JsonRpcClientModule::Setup(ibmw::Runtime core) {
  core_ = core;
  service_ref_ = core_.GetService();
  bool ret_compute = ibmw::json::rpc::RegisterClientFunc<ComputeRequest, ComputeResponse>(
      service_ref_, kServiceName, "Compute");
  if (!ret_compute) {
    MW_ERROR("Failed to register client func for {}/Compute", kServiceName);
    return false;
  }

  MW_INFO("JsonRpcClientModule::Setup succeeded.");
  return true;
}

bool JsonRpcClientModule::Run() {
  running_ = true;
  std::thread([this]() { RpcLoop(); }).detach();
  MW_INFO("JsonRpcClientModule::Run — client loop started.");
  return true;
}

void JsonRpcClientModule::Stop() {
  running_ = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  MW_INFO("JsonRpcClientModule::Stop.");
}

IBMW_TS_RPC_CLIENT_LOOP_IMPL(JsonRpcClientModule)

}  // namespace my_project
