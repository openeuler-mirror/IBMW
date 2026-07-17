// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"

#include "subscriber_module/subscriber_module.h"

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
    std::cerr << "Usage: " << argv[0] << " <subscriber.yaml>\n";
    return -1;
  }

  try {
    Engine engine;
    g_engine_ptr = &engine;

    ibmw::samples::transport_benchmark::iox_loan_v2::SubscriberModule sub;
    engine.GetModuleBus().RegisterModule(sub.NativeHandle());

    Engine::Options options;
    options.cfg_file_path = argv[1];
    engine.Initialize(options);

    // No driver injection: SubscriberModule attaches via the high-level
    // LoanedSubscriber<T>::AttachBytes() fast lane in Setup(). The
    // dispatcher routes AttachLoaned to iceoryx automatically based on
    // the subscriber.yaml routing rule.
    engine.Start();
    engine.Shutdown();
    g_engine_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "iox_loan_v2 subscriber exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
