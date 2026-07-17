// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"
#include "../../managed/subscriber_module/subscriber_module.h"

using namespace ibmw::runtime::core;
using namespace ibmw::examples::loaned_message_demo;

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

  try {
    Engine core;
    global_core_ptr = &core;

    SubscriberModule subscriber;
    core.GetModuleBus().RegisterModule(subscriber.NativeHandle());

    Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    } else {
      std::cerr << "Usage: " << argv[0] << " <config_file_path>\n";
      return -1;
    }

    core.Initialize(options);
    core.Start();
    core.Shutdown();
    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return -1;
  }

  return 0;
}
