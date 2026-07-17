// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// net_send_tcp_publisher_app: standalone publisher process for the
// native-net Send-over-raw-TCP baseline benchmark. Pair with
// net_send_tcp_subscriber_app (see start_net_send_tcp.sh). No
// iox-roudi, no FastDDS -- the net_extension's TCP driver opens a
// plain asio TCP socket to the configured server_url_list[0].
//
// No driver pointer needs to be injected into the module: the publish
// path goes through `ibmw::transport::Send()`, which the engine
// routes to the net_extension's tcp transport driver via the yaml
// `messaging.publishers` rule. The driver CDR-serializes via the
// MessageTypeSupport "cdr" registration, then writes a custom binary
// frame (pattern + serialization_type + ctx-meta + payload bytes)
// over the TCP socket.

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

    ibmw::samples::transport_benchmark::net_send_tcp::PublisherModule pub;
    engine.GetModuleBus().RegisterModule(pub.NativeHandle());

    Engine::Options options;
    options.cfg_file_path = argv[1];
    engine.Initialize(options);

    engine.Start();
    engine.Shutdown();
    g_engine_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "net_send_tcp publisher exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
