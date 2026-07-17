// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

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
};

int32_t main(int32_t argc, char** argv) {
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  std::cout << "ibmw start." << std::endl;

  try {
    Engine core;
    global_core_ptr = &core;

    // Initialize
    Engine::Options options;
    if (argc > 1) options.cfg_file_path = argv[1];

    core.Initialize(options);

    // Create Module
    ibmw::Runtime module_handle(
        core.GetModuleBus().CreateModule("HelloWorldModule"));
    MW_LOG(module_handle.GetLogger(), ibmw::common::util::kLogLevelInfo, "This is an example log.");

    // Read cfg
    auto file_path = module_handle.GetConfigurator().GetConfigFilePath();
    if (!file_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(file_path));
      for (const auto& itr : cfg_node) {
        std::string k = itr.first.as<std::string>();
        std::string v = itr.second.as<std::string>();
        MW_LOG(module_handle.GetLogger(), ibmw::common::util::kLogLevelInfo, "cfg [{} : {}]", k, v);
      }
    }

    // Start
    auto fu = core.AsyncStart();

    MW_LOG(module_handle.GetLogger(), ibmw::common::util::kLogLevelInfo, "Run succeeded.");

    // Wait
    fu.wait();

    // Shutdown
    core.Shutdown();

    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cout << "ibmw run with exception and exit. " << e.what()
              << std::endl;
    return -1;
  }

  std::cout << "ibmw exit." << std::endl;

  return 0;
}
