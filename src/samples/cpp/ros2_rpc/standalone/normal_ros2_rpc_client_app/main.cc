// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <csignal>
#include <iostream>

#include "engine/engine.h"

#include "cpp/core.h"

#include "RosTestRpc.ibmw_rpc.srv.h"

using namespace ibmw::runtime::core;

bool run_flag = true;

void SignalHandler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    run_flag = false;
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

    // Initialize
    Engine::Options options;
    if (argc > 1) options.cfg_file_path = argv[1];

    core.Initialize(options);

    // Create Module
    ibmw::Runtime module_handle(
        core.GetModuleBus().CreateModule("NormalRpcSyncClientModule"));

    double rpc_frq = 0.5;

    // Read cfg
    auto file_path = module_handle.GetConfigurator().GetConfigFilePath();
    if (!file_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(file_path));
      if (cfg_node["rpc_frq"]) {
        rpc_frq = cfg_node["rpc_frq"].as<double>();
      }
    }

    // Get service handle
    auto rpc_handle = module_handle.GetService();
    MW_CHECK_LOG_THROW(module_handle.GetLogger(), rpc_handle, ibmw::common::util::kLogLevelError, "Get service handle failed.");

    // Register rpc client
    bool ret = example_ros2::srv::RegisterRosTestRpcClientFunc(rpc_handle);
    MW_CHECK_LOG_THROW(module_handle.GetLogger(), ret, ibmw::common::util::kLogLevelError, "Register client failed.");

    // Create client access
    example_ros2::srv::RosTestRpcSyncAccess access(rpc_handle);

    // Start
    auto fu = core.AsyncStart();

    // Call RPC Service
    uint32_t count = 0;
    while (run_flag) {
      std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<uint32_t>(1000 / rpc_frq)));

      count++;
      MW_LOG(module_handle.GetLogger(), ibmw::common::util::kLogLevelInfo, "Loop count : {} -------------------------", count);

      // call rpc
      example_ros2::srv::RosTestRpc_Request req;
      example_ros2::srv::RosTestRpc_Response rsp;
      req.data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

      auto ctx_ptr = access.NewContextSharedPtr();
      ctx_ptr->SetTimeout(std::chrono::seconds(3));

      MW_LOG(module_handle.GetLogger(), ibmw::common::util::kLogLevelInfo,
                    "start new service call. req:\n{}", example_ros2::srv::to_yaml(req));

      auto status = access.RosTestRpc(ctx_ptr, req, rsp);

      if (status.OK()) {
        MW_LOG(module_handle.GetLogger(), ibmw::common::util::kLogLevelInfo,
                      "Client get rpc ret, status: {}, rsp: {}",
                      status.ToString(), example_ros2::srv::to_yaml(rsp));
      } else {
        MW_LOG(module_handle.GetLogger(), ibmw::common::util::kLogLevelWarn,
                      "Client get rpc error ret, status: {}", status.ToString());
      }
    }

    // Shutdown
    core.Shutdown();

    // Wait
    fu.wait();
  } catch (const std::exception& e) {
    std::cout << "Ibmw run with exception and exit. " << e.what()
              << std::endl;
    return -1;
  }

  std::cout << "Ibmw exit." << std::endl;

  return 0;
}
