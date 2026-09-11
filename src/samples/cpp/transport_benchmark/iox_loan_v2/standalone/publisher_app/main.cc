// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// iox_loan_v2_publisher_app: standalone publisher process for the
// IBMW user-level zero-copy loan benchmark. Pair with
// iox_loan_v2_subscriber_app and a running iox-roudi daemon
// (see start_iox_loan_v2.sh).
//
// The iceoryx driver is looked up through GetTransportBus() and
// injected into the module so PublisherModule can register POD topics before
// Engine::Start(). The publish hot loop itself stays on the public
// SenderRef::Loan / PublishLoaned API -- no driver-pointer access in
// the per-tick path.

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"

// IBMW_R1_EXEMPT_BEGIN
#include "iceoryx_ext/iceoryx_transport_driver.h"
// IBMW_R1_EXEMPT_END
#include "publisher_module/publisher_module.h"

using namespace ibmw::runtime::core;
namespace iox_ext = ibmw::extensions::iceoryx_extension;

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

    ibmw::samples::transport_benchmark::iox_loan_v2::PublisherModule pub;
    engine.GetModuleBus().RegisterModule(pub.NativeHandle());

    Engine::Options options;
    options.cfg_file_path = argv[1];
    engine.Initialize(options);

    iox_ext::IceoryxTransportDriver* driver = nullptr;
    for (auto* td : engine.GetTransportBus().GetUsedTransportDriver()) {
      if (td->Name() == "iceoryx") {
        driver = dynamic_cast<iox_ext::IceoryxTransportDriver*>(td);
        break;
      }
    }
    if (driver == nullptr) {
      std::cerr << "iceoryx driver not found in TransportBus\n";
      return -1;
    }
    pub.SetIceoryxDriver(driver);
    if (!pub.PreparePodTopics()) {
      std::cerr << "failed to prepare iceoryx POD topics\n";
      return -1;
    }

    engine.Start();
    engine.Shutdown();
    g_engine_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "iox_loan_v2 publisher exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
