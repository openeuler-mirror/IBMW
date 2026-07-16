// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// net_send_tcp_subscriber_app: standalone subscriber process for the
// native-net Send-over-raw-TCP baseline benchmark. The subscriber
// path is identical to dds_send_shm / dds_send_tcp (standard
// OnMessage<T> through the engine), so no driver pointer is injected.
// The net_extension's tcp driver owns the TCP listener (configured
// via tcp_options.listen_port in subscriber.yaml).

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

    ibmw::samples::transport_benchmark::net_send_tcp::SubscriberModule sub;
    engine.GetModuleBus().RegisterModule(sub.NativeHandle());

    Engine::Options options;
    options.cfg_file_path = argv[1];
    engine.Initialize(options);

    engine.Start();
    engine.Shutdown();
    g_engine_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "net_send_tcp subscriber exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
