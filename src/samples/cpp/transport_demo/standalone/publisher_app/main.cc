// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"
#include "publisher_module/publisher_module.h"

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

  std::cout << "Ibmw Pub/Sub Demo - Publisher (App Mode) start." << std::endl;

  try {
    Engine core;
    global_core_ptr = &core;

    // Register publisher module only
    pubsub_demo::PublisherModule publisher_module;
    core.GetModuleBus().RegisterModule(publisher_module.NativeHandle());

    // Initialize
    Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    } else {
      std::cerr << "Usage: " << argv[0] << " <config_file_path>" << std::endl;
      return -1;
    }

    core.Initialize(options);

    // Start
    std::cout << "Starting Ibmw Core..." << std::endl;
    core.Start();

    // Shutdown
    core.Shutdown();

    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cout << "Ibmw run with exception and exit. " << e.what()
              << std::endl;
    return -1;
  }

  std::cout << "Ibmw Pub/Sub Demo - Publisher (App Mode) exit." << std::endl;

  return 0;
}
