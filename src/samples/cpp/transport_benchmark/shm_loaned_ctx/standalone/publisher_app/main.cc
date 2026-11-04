// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// shm_loaned_ctx_publisher_app: standalone publisher of the shm_loaned_ctx
// benchmark (pair with shm_loaned_ctx_subscriber_app, see
// start_shm_loaned_ctx.sh). The driver is looked up by name after
// engine.Initialize() -- the shm extension registers it as "iceoryx"
// (driver_name option) -- and injected as a plain TransportDriver*.

#include <chrono>
#include <csignal>
#include <future>
#include <iostream>
#include <thread>

#include "cpp/core.h"
#include "engine/engine.h"

#include "publisher_module/publisher_module.h"

using namespace ibmw::runtime::core;

volatile sig_atomic_t g_shutdown_requested = 0;

void SignalHandler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) g_shutdown_requested = 1;
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

    // Shutdown (and the final Core Metrics snapshot) runs on the main thread,
    // never in the signal handler. The loop also ends when the engine stops on
    // its own; the Shutdown() below is then a no-op.
    auto stopped = engine.AsyncStart();
    while (!g_shutdown_requested &&
           stopped.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    engine.Shutdown();
    stopped.get();
  } catch (const std::exception& e) {
    std::cerr << "shm_loaned_ctx publisher exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
