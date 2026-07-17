// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// App-mode entry point for executor_real_time_app.

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"

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

  std::cout << "executor_real_time_app start." << std::endl;

  try {
    Engine core;
    global_core_ptr = &core;

    // Initialize
    Engine::Options options;
    if (argc > 1) options.cfg_file_path = argv[1];
    core.Initialize(options);

    // Create module handle (module name must match YAML config)
    ibmw::Runtime module_handle(
        core.GetModuleBus().CreateModule("RealTimeModule"));

    MW_LOG(module_handle.GetLogger(),
           ibmw::common::util::kLogLevelInfo,
           "App mode started successfully.");

    // Start the engine asynchronously
    auto fu = core.AsyncStart();

    MW_LOG(module_handle.GetLogger(),
           ibmw::common::util::kLogLevelInfo,
           "Engine is running. Press Ctrl+C to stop.");

    // Wait for shutdown signal
    fu.wait();

    // Clean shutdown
    core.Shutdown();
    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return -1;
  }

  std::cout << "executor_real_time_app exit." << std::endl;
  return 0;
}
