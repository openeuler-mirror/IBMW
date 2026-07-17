// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"
#include "../../managed/subscriber_module/subscriber_module.h"

using namespace ibmw::runtime::core;
using namespace ibmw::examples::ros2_interop_demo;

Engine* global_core_ptr = nullptr;

void SignalHandler(int sig) {
  if (global_core_ptr && (sig == SIGINT || sig == SIGTERM)) {
    global_core_ptr->Shutdown();
    return;
  }
  raise(sig);
}

int main(int argc, char** argv) {
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  std::cout << "========================================\n";
  std::cout << "Ibmw Subscriber Only (for ROS2 interop)\n";
  std::cout << "========================================\n";

  try {
    Engine core;
    global_core_ptr = &core;

    // Register ONLY subscriber module
    SubscriberModule subscriber_module;
    core.GetModuleBus().RegisterModule(subscriber_module.NativeHandle());

    // Initialize
    Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    } else {
      std::cerr << "Usage: " << argv[0] << " <config_file_path>\n";
      return -1;
    }

    core.Initialize(options);

    // Start
    std::cout << "Starting Ibmw Subscriber...\n";
    std::cout << "Listening on /chatter (DDS topic: rt/chatter)\n";
    std::cout << "\nPress Ctrl+C to exit\n";
    std::cout << "========================================\n";

    core.Start();

    // Shutdown
    core.Shutdown();
    global_core_ptr = nullptr;

  } catch (const std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return -1;
  }

  std::cout << "\nIbmw Subscriber exit.\n";
  return 0;
}
