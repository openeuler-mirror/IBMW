// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "normal_rpc_sync_server_module/normal_rpc_sync_server_module.h"
#include "normal_rpc_sync_server_module/global.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_sync_server_module {

bool NormalRpcSyncServerModule::Setup(ibmw::Runtime core) {
  core_ = core;

  SetLogger(core_.GetLogger());

  try {
    // Create service
    service_ptr_ = std::make_shared<RosTestRpcSyncServiceImpl>();

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

bool NormalRpcSyncServerModule::Run() { return true; }

void NormalRpcSyncServerModule::Stop() {}

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_sync_server_module
