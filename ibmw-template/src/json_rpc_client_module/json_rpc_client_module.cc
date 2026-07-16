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
static constexpr const char* kFuncName = "Compute";

bool JsonRpcClientModule::Setup(ibmw::Runtime core) {
  core_ = core;
  service_ref_ = core_.GetService();

  // One-line typed client registration — no manual GetFullFuncName, no
  // manual GetJsonMessageTypeSupport
  bool ret = ibmw::json::rpc::RegisterClientFunc<ComputeRequest, ComputeResponse>(
      service_ref_, kServiceName, kFuncName);
  if (!ret) {
    MW_ERROR("Failed to register client func for {}/{}", kServiceName, kFuncName);
    return false;
  }

  MW_INFO("JsonRpcClientModule::Setup succeeded — client registered for {}/{}.",
          kServiceName, kFuncName);
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
  // Test data: pairs of (operation, a, b)
  struct TestCase {
    const char* op;
    double a;
    double b;
  };

  static constexpr TestCase kTestCases[] = {
      {"add", 10.0, 25.5},
      {"multiply", 3.0, 7.0},
      {"subtract", 100.0, 42.0},
      {"unknown_op", 1.0, 2.0},
  };

  uint32_t round = 0;

  while (running_) {
    for (const auto& tc : kTestCases) {
      if (!running_) break;

      ComputeRequest req;
      req.operation = tc.op;
      req.operand_a = tc.a;
      req.operand_b = tc.b;

      ComputeResponse rsp;

      // One-line typed synchronous call — no manual Context, no promise/future,
      // no void*, no GetFullFuncName
      auto status = ibmw::json::rpc::Call<ComputeRequest, ComputeResponse>(
          service_ref_, kServiceName, kFuncName, req, rsp);

      MW_INFO("[Round {}] Compute({}, {:.1f}, {:.1f}) => code={}, msg='{}', result={:.1f}, status={}",
              round, req.operation, req.operand_a, req.operand_b,
              rsp.code, rsp.msg, rsp.result, status.Code());

      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    ++round;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace my_project
