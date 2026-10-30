// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// shm_loaned_ctx_publisher_app: standalone publisher of the shm_loaned_ctx
// benchmark (pair with shm_loaned_ctx_subscriber_app, see
// start_shm_loaned_ctx.sh). The driver is looked up by name after
// engine.Initialize() -- the shm extension registers it as "iceoryx"
// (driver_name option) -- and injected as a plain TransportDriver*.

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"

#include "publisher_module/publisher_module.h"

using namespace ibmw::runtime::core;

Engine* g_engine_ptr = nullptr;

void SignalHandler(int sig) {
  if (g_engine_ptr && (sig == SIGINT || sig == SIGTERM)) {
    g_engine_ptr->Shutdown();
    return;
  }
  raise(sig);
}

int main(int argc, char** argv) {
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  if (argc <= 1) {
    std::cerr << "Usage: " << argv[0] << " <publisher.yaml>\n";
    return -1;
  }

  try {
    Engine engine;
    g_engine_ptr = &engine;

    ibmw::samples::transport_benchmark::shm_loaned_ctx::PublisherModule pub;
    engine.GetModuleBus().RegisterModule(pub.NativeHandle());

    Engine::Options options;
    options.cfg_file_path = argv[1];
    engine.Initialize(options);

    transport::TransportDriver* driver = nullptr;
    for (auto* td : engine.GetTransportBus().GetUsedTransportDriver()) {
      if (td->Name() == "iceoryx") {
        driver = td;
        break;
      }
    }
    if (driver == nullptr) {
      std::cerr << "driver \"iceoryx\" not found in TransportBus\n";
      return -1;
    }
    pub.SetDriver(driver);

    engine.Start();
    engine.Shutdown();
    g_engine_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "shm_loaned_ctx publisher exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
