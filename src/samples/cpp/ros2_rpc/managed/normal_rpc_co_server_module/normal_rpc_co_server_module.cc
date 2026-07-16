// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "normal_rpc_co_server_module/normal_rpc_co_server_module.h"
#include "normal_rpc_co_server_module/filter.h"
#include "normal_rpc_co_server_module/global.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_co_server_module {

bool NormalRpcCoServerModule::Setup(ibmw::Runtime core) {
  core_ = core;

  SetLogger(core_.GetLogger());

  try {
    // Create service
    service_ptr_ = std::make_shared<RosTestRpcServiceImpl>();

    // Register filter
    service_ptr_->RegisterFilter(TimeCostLogServerFilter);

    // Register service
    bool ret = core_.GetService().RegisterService(service_ptr_.get());
    MW_REQUIRE(ret, "Register service failed.");

    MW_INFO("Register service succeeded.");

  } catch (const std::exception& e) {
    MW_ERROR("Init failed, {}", e.what());
    return false;
  }

  MW_INFO("Setup succeeded.");

  return true;
}

bool NormalRpcCoServerModule::Run() { return true; }

void NormalRpcCoServerModule::Stop() {}

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_co_server_module
