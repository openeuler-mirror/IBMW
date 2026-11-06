// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// iceoryx_interproc_subscriber_app: standalone subscriber process for the
// two-process Iceoryx demo. Start this BEFORE the publisher so the
// iceoryx Listener has its callback wired by the time the first sample
// arrives (otherwise the early samples are dropped by RouDi history depth).

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"
#include "subscriber_module/subscriber_module.h"

using namespace ibmw::runtime::core;

// shm_interproc_demo builds this file unchanged over the self-developed shm transport
// and overrides the two banner strings below (see shm_interproc_demo/CMakeLists.txt).
#ifndef IBMW_DEMO_TITLE
#define IBMW_DEMO_TITLE "IBMW Iceoryx Interproc Demo"
#endif
#ifndef IBMW_DEMO_TRANSPORT_NOTE
#define IBMW_DEMO_TRANSPORT_NOTE "NOTE: requires iox-roudi daemon to be running."
#endif

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

  std::cout << IBMW_DEMO_TITLE " (Subscriber) start." << std::endl;
  std::cout << IBMW_DEMO_TRANSPORT_NOTE << std::endl;

  try {
    Engine core;
    global_core_ptr = &core;

    iceoryx_interproc_demo::SubscriberModule subscriber_module;
    core.GetModuleBus().RegisterModule(subscriber_module.NativeHandle());

    Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    } else {
      std::cerr << "Usage: " << argv[0] << " <config_file_path>" << std::endl;
      return -1;
    }

    core.Initialize(options);

    std::cout << "Starting IBMW Engine (subscriber)..." << std::endl;
    core.Start();

    core.Shutdown();
    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cout << "IBMW subscriber exception: " << e.what() << std::endl;
    return -1;
  }

  std::cout << IBMW_DEMO_TITLE " (Subscriber) exit." << std::endl;
  return 0;
}
