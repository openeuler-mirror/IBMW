// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// App-mode entry point for Demo 8 (bring-your-own IDL pub/sub).

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"
#include "my_msg_publisher_module/my_msg_publisher_module.h"
#include "my_msg_subscriber_module/my_msg_subscriber_module.h"

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

  std::cout << "my_msg_pubsub_app start." << std::endl;

  try {
    Engine core;
    global_core_ptr = &core;

    my_project::MyMsgPublisherModule publisher_module;
    my_project::MyMsgSubscriberModule subscriber_module;

    core.GetModuleBus().RegisterModule(publisher_module.NativeHandle());
    core.GetModuleBus().RegisterModule(subscriber_module.NativeHandle());

    Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    } else {
      std::cerr << "Usage: " << argv[0] << " <config.yaml>" << std::endl;
      return -1;
    }

    core.Initialize(options);
    std::cout << "Starting engine..." << std::endl;
    core.Start();
    core.Shutdown();
    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return -1;
  }

  std::cout << "my_msg_pubsub_app exit." << std::endl;
  return 0;
}
