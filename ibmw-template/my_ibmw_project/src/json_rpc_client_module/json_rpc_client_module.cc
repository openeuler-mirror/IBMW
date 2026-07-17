// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "json_rpc_client_module/json_rpc_client_module.h"

#include <chrono>
#include <thread>

#include "cpp/util/json_rpc_helpers.h"
#include "json_custom_types.h"

namespace my_project {

// Service and function naming constants
static constexpr const char* kServiceName = "my_project.ComputeService";

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

void JsonRpcClientModule::RpcLoop() {
  uint32_t round = 0;

  while (running_) {
      {
        ComputeRequest req;
        ComputeResponse rsp;
        // TODO: populate req fields

        auto status = ibmw::json::rpc::Call<ComputeRequest, ComputeResponse>(
            service_ref_, kServiceName, "Compute", req, rsp);

        MW_INFO("[Round {}] Compute => status={}", round, status.Code());

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }

    ++round;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace my_project
