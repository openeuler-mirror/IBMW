// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// iceoryx_interproc_publisher_app: standalone publisher process for the
// two-process Iceoryx demo. Pair with iceoryx_interproc_subscriber_app and
// a running iox-roudi daemon (see start_iceoryx_interproc.sh).

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"
#include "publisher_module/publisher_module.h"

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

  std::cout << IBMW_DEMO_TITLE " (Publisher) start." << std::endl;
  std::cout << IBMW_DEMO_TRANSPORT_NOTE << std::endl;

  try {
    Engine core;
    global_core_ptr = &core;

    iceoryx_interproc_demo::PublisherModule publisher_module;
    core.GetModuleBus().RegisterModule(publisher_module.NativeHandle());

    Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    } else {
      std::cerr << "Usage: " << argv[0] << " <config_file_path>" << std::endl;
      return -1;
    }

    core.Initialize(options);

    std::cout << "Starting IBMW Engine (publisher)..." << std::endl;
    core.Start();

    core.Shutdown();
    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cout << "IBMW publisher exception: " << e.what() << std::endl;
    return -1;
  }

  std::cout << IBMW_DEMO_TITLE " (Publisher) exit." << std::endl;
  return 0;
}
