// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <csignal>
#include <iostream>

#include "engine/engine.h"
#include "helloworld_module/helloworld_module.h"

using namespace ibmw::runtime::core;
using namespace ibmw::examples::cpp::helloworld::helloworld_module;

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

    // Register module
    HelloWorldModule helloworld_module;
    core.GetModuleBus().RegisterModule(helloworld_module.NativeHandle());

    Engine::Options options;
    if (argc > 1) options.cfg_file_path = argv[1];

    core.Initialize(options);

    core.Start();

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
