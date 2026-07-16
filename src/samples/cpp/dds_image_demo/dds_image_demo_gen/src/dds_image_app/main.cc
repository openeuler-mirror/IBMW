// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// App-mode entry point for dds_image_app.
//
// This main.cc contains no driver-injection boilerplate even when
// loaned_publisher modules are present. The framework's
// ModuleBus::BroadcastDriverSlot() runs between Initialize() and Start()
// and dispatches each used transport driver pointer into every registered
// module via Module::SetDriverSlot(slot_id, ptr). Loaned modules use
// IBMW_USE_DRIVER to capture the iceoryx / dds driver pointer they need
// (compile-time typed slot match, no string compare).

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"
#include "image_publisher_module/image_publisher.h"
#include "image_subscriber_module/image_subscriber.h"

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

  std::cout << "dds_image_app start." << std::endl;

  try {
    Engine core;
    global_core_ptr = &core;

    // Register modules
    dds_image_demo::ImagePublisher module_0;
    dds_image_demo::ImageSubscriber module_1;

    core.GetModuleBus().RegisterModule(module_0.NativeHandle());
    core.GetModuleBus().RegisterModule(module_1.NativeHandle());

    // Initialize
    Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    } else {
      std::cerr << "Usage: " << argv[0] << " <config.yaml>" << std::endl;
      return -1;
    }

    core.Initialize(options);

    // Start (blocking). The framework auto-injects transport drivers into
    // any module that overrides Module::SetDriverSlot (typically via the
    // IBMW_USE_DRIVER macro) — no boilerplate needed.
    std::cout << "Starting engine..." << std::endl;
    core.Start();

    // Shutdown
    core.Shutdown();
    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return -1;
  }

  std::cout << "dds_image_app exit." << std::endl;
  return 0;
}
