// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <csignal>
#include <chrono>
#include <iostream>
#include <thread>

#include "cpp/core.h"
#include "engine/engine.h"

// IBMW_R1_EXEMPT_BEGIN
#include "iceoryx_ext/iceoryx_transport_driver.h"
// IBMW_R1_EXEMPT_END
#include "subscriber_module/subscriber_module.h"

using namespace ibmw::runtime::core;
namespace iox_ext = ibmw::extensions::iceoryx_extension;

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

    iox_ext::IceoryxTransportDriver* driver = nullptr;
    for (auto* td : engine.GetTransportBus().GetUsedTransportDriver()) {
      if (td->Name() == "iceoryx") {
#if defined(__OHOS__) || defined(__OHOS)
        // OHOS hides RTTI identity across this plugin boundary.
        driver = static_cast<iox_ext::IceoryxTransportDriver*>(td);
#else
        driver = dynamic_cast<iox_ext::IceoryxTransportDriver*>(td);
#endif
        break;
      }
    }
    if (driver == nullptr) {
      std::cerr << "iceoryx driver not found in TransportBus\n";
      return -1;
    }
    sub.SetIceoryxDriver(driver);

    std::cout << "PERF_SUBSCRIBER_READY" << std::endl;
    auto stopped = engine.AsyncStart();
    while (!g_shutdown_requested) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    engine.Shutdown();
    stopped.get();
  } catch (const std::exception& e) {
    std::cerr << "iox_loaned_ctx subscriber exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
