// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// shm_loaned_pod_publisher_app: standalone publisher process for the
// zero-copy POD benchmark. Pair with shm_loaned_pod_subscriber_app
// (see start_shm_loaned_pod.sh; no daemon needed).
//
// The shm driver is looked up through GetTransportBus() and injected
// into the module after engine.Initialize() returns -- mirrors the DDS
// loaned_message_demo's plain_pub_only_app pattern.

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"

// IBMW_R1_EXEMPT_BEGIN
#include "shm_ext/shm_transport_driver.h"
// IBMW_R1_EXEMPT_END
#include "publisher_module/publisher_module.h"

using namespace ibmw::runtime::core;
namespace shm_ext = ibmw::extensions::shm_extension;

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

    ibmw::samples::transport_benchmark::shm_loaned_pod::PublisherModule pub;
    engine.GetModuleBus().RegisterModule(pub.NativeHandle());

    Engine::Options options;
    options.cfg_file_path = argv[1];
    engine.Initialize(options);

    shm_ext::ShmTransportDriver* driver = nullptr;
    for (auto* td : engine.GetTransportBus().GetUsedTransportDriver()) {
      if (td->Name() == "iceoryx") {  // shm_ext registers under driver_name
        driver = dynamic_cast<shm_ext::ShmTransportDriver*>(td);
        break;
      }
    }
    if (driver == nullptr) {
      std::cerr << "shm driver not found in TransportBus\n";
      return -1;
    }
    pub.SetShmDriver(driver);
    if (!pub.RegisterWithDriver()) {
      std::cerr << "shm_loaned_pod publisher: POD registration failed\n";
      return -1;
    }

    engine.Start();
    engine.Shutdown();
    g_engine_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "shm_loaned_pod publisher exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
