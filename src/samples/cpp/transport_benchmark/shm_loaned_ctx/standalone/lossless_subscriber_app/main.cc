// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// shm_loaned_ctx_lossless_subscriber_app: receive side of the shm core_metrics
// gate. Runs iox_loaned_ctx's subscriber module (shared measurement window,
// status schema, PERF_SUBSCRIBER_RUNNING) on the self-developed shm transport;
// bench_options.topic selects this lane's topic.

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include "cpp/core.h"
#include "engine/engine.h"

#include "subscriber_module/subscriber_module.h"

using namespace ibmw::runtime::core;

volatile sig_atomic_t g_shutdown_requested = 0;

void SignalHandler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) g_shutdown_requested = 1;
}

int main(int argc, char** argv) {
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  if (argc <= 1) {
    std::cerr << "Usage: " << argv[0] << " <subscriber.yaml>\n";
    return -1;
  }

  try {
    Engine engine;
    ibmw::samples::transport_benchmark::iox_loaned_ctx::SubscriberModule sub;
    engine.GetModuleBus().RegisterModule(sub.NativeHandle());

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
    sub.SetDriver(driver);

    std::cout << "PERF_SUBSCRIBER_READY" << std::endl;
    auto stopped = engine.AsyncStart();
    while (!g_shutdown_requested) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    engine.Shutdown();
    stopped.get();
  } catch (const std::exception& e) {
    std::cerr << "shm_loaned_ctx lossless subscriber exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
