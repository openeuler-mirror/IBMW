// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// App-mode entry point for dds_rpc_server_app.
//
// Managed modules use public transport access handles. Concrete driver
// injection is reserved for explicitly direct transport modules.

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"
#include "rpc_server_module/rpc_server_module.h"

using namespace ibmw::runtime::core;

Engine* global_core_ptr = nullptr;

void SignalHandler(int sig) {
  if (global_core_ptr && (sig == SIGINT || sig == SIGTERM)) {
    global_core_ptr->Shutdown();
    return;
  }
  raise(sig);
}

int32_t main(int32_t argc, char** argv) {
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  std::cout << "dds_rpc_server_app start." << std::endl;

  try {
    Engine core;
    global_core_ptr = &core;

    // Register modules
    dds_rpc_demo::RpcServerModule module_0;

    core.GetModuleBus().RegisterModule(module_0.NativeHandle());

    // Initialize
    Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    } else {
      std::cerr << "Usage: " << argv[0] << " <config.yaml>" << std::endl;
      return -1;
    }

    core.Initialize(options);

    // Start (blocking). Runtime configuration selects concrete transports.
    std::cout << "Starting engine..." << std::endl;
    core.Start();

    // Shutdown
    core.Shutdown();
    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return -1;
  }

  std::cout << "dds_rpc_server_app exit." << std::endl;
  return 0;
}
