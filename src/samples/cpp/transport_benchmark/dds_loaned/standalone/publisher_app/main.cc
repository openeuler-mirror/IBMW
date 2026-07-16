// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// dds_loaned_publisher_app: standalone publisher process for the DDS
// LoanedMessage zero-copy benchmark. Pair with dds_loaned_subscriber_app
// (no roudi needed -- FastDDS DataSharing uses its own SHM segment).
//
// The dds driver is looked up through GetTransportBus() and injected
// into the module after engine.Initialize() returns -- mirrors the
// loaned_message_demo plain_pub_only_app pattern, plus the iox_loaned_*
// benches' pattern for iceoryx.

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"

// IBMW_R1_EXEMPT_BEGIN
#include "dds_ext/dds_transport_driver.h"
// IBMW_R1_EXEMPT_END
#include "publisher_module/publisher_module.h"

using namespace ibmw::runtime::core;
namespace dds_ext = ibmw::extensions::dds_extension;

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

    ibmw::samples::transport_benchmark::dds_loaned::PublisherModule pub;
    engine.GetModuleBus().RegisterModule(pub.NativeHandle());

    Engine::Options options;
    options.cfg_file_path = argv[1];
    engine.Initialize(options);

    dds_ext::DdsTransportDriver* driver = nullptr;
    for (auto* td : engine.GetTransportBus().GetUsedTransportDriver()) {
      if (td->Name() == "dds") {
        driver = dynamic_cast<dds_ext::DdsTransportDriver*>(td);
        break;
      }
    }
    if (driver == nullptr) {
      std::cerr << "dds driver not found in TransportBus\n";
      return -1;
    }
    pub.SetDdsDriver(driver);

    engine.Start();
    engine.Shutdown();
    g_engine_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "dds_loaned publisher exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
