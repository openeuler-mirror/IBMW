// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// iox_send_publisher_app: standalone publisher process for the
// SHM-but-no-loaned baseline benchmark. Pair with
// iox_send_subscriber_app and a running iox-roudi daemon (see
// start_iox_send.sh).
//
// Unlike iox_loaned_ctx, no driver pointer needs to be injected into
// the module: the publish path goes through `ibmw::transport::Send()`,
// which the engine routes to the iceoryx driver via the yaml
// `messaging.publishers` rule. The driver internally borrows + memcpys
// into an SHM chunk -- no extension-specific API touched in user code.

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

    ibmw::samples::transport_benchmark::iox_send::PublisherModule pub;
    engine.GetModuleBus().RegisterModule(pub.NativeHandle());

    Engine::Options options;
    options.cfg_file_path = argv[1];
    engine.Initialize(options);

    engine.Start();
    engine.Shutdown();
    g_engine_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "iox_send publisher exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
