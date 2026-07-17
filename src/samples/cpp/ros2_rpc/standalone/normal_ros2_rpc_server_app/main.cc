// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <csignal>
#include <iostream>

#include "engine/engine.h"

#include "cpp/core.h"

#include "RosTestRpc.ibmw_rpc.srv.h"

using namespace ibmw::runtime::core;

class RosTestRpcSyncServiceImpl : public example_ros2::srv::RosTestRpcSyncService {
 public:
  explicit RosTestRpcSyncServiceImpl(ibmw::Runtime module_handle) : module_handle_(module_handle) {}
  ~RosTestRpcSyncServiceImpl() override = default;

  ibmw::rpc::Status RosTestRpc(
      ibmw::rpc::ContextRef ctx,
      const example_ros2::srv::RosTestRpc_Request& req,
      example_ros2::srv::RosTestRpc_Response& rsp) override {
    rsp.code = 123;

    MW_LOG(module_handle_.GetLogger(), ibmw::common::util::kLogLevelInfo,
                  "Get new service call. context: {}, req:\n{}",
                  ctx.ToString(), example_ros2::srv::to_yaml(req));

    return ibmw::rpc::Status();
  }

 private:
  ibmw::Runtime module_handle_;
};

Engine* global_core_ptr = nullptr;

void SignalHandler(int sig) {
  if (global_core_ptr && (sig == SIGINT || sig == SIGTERM)) {
    global_core_ptr->Shutdown();
    return;
  }

  raise(sig);
};

int32_t main(int32_t argc, char** argv) {
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  std::cout << "Ibmw start." << std::endl;

  try {
    Engine core;
    global_core_ptr = &core;

    // Initialize
    Engine::Options options;
    if (argc > 1) options.cfg_file_path = argv[1];

    core.Initialize(options);

    // Create Module
    ibmw::Runtime module_handle(
        core.GetModuleBus().CreateModule("NormalRpcSyncServerModule"));

    // Create service
    auto service_ptr = std::make_shared<RosTestRpcSyncServiceImpl>(module_handle);

    // Register service
    bool ret = module_handle.GetService().RegisterService(service_ptr.get());
    MW_CHECK_LOG_THROW(module_handle.GetLogger(), ret, ibmw::common::util::kLogLevelError, "Register service failed.");

    MW_LOG(module_handle.GetLogger(), ibmw::common::util::kLogLevelInfo, "Register service succeeded.");

    // Start
    core.Start();

    // Shutdown
    core.Shutdown();

    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cout << "Ibmw run with exception and exit. " << e.what()
              << std::endl;
    return -1;
  }

  std::cout << "Ibmw exit." << std::endl;

  return 0;
}
