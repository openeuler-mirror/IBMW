// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"
// IBMW_R1_EXEMPT_BEGIN
#include "dds_ext/dds_transport_driver.h"
// IBMW_R1_EXEMPT_END
#include "../../managed/plain_publisher_module/plain_publisher_module.h"
#include "../../managed/nonplain_publisher_module/nonplain_publisher_module.h"
#include "../../managed/subscriber_module/subscriber_module.h"

using namespace ibmw::runtime::core;
using namespace ibmw::examples::loaned_message_demo;
namespace dds_ext = ibmw::extensions::dds_extension;

Engine* global_core_ptr = nullptr;

void SignalHandler(int sig) {
  if (global_core_ptr && (sig == SIGINT || sig == SIGTERM)) {
    global_core_ptr->Shutdown();
    return;
  }
  raise(sig);
}

int main(int argc, char** argv) {
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  std::cout << "========================================\n";
  std::cout << "LoanedMessage End-to-End Demo\n";
  std::cout << "========================================\n";

  try {
    Engine core;
    global_core_ptr = &core;

    PlainPublisherModule plain_pub;
    NonPlainPublisherModule nonplain_pub;
    SubscriberModule subscriber;

    core.GetModuleBus().RegisterModule(plain_pub.NativeHandle());
    core.GetModuleBus().RegisterModule(nonplain_pub.NativeHandle());
    core.GetModuleBus().RegisterModule(subscriber.NativeHandle());

    Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    } else {
      std::cerr << "Usage: " << argv[0] << " <config_file_path>\n";
      return -1;
    }

    core.Initialize(options);

    // Find the DDS transport driver from the engine's used transport drivers.
    // We cannot use GetDdsTransportDriver() because it reads from a file-static
    // global that lives in the statically-linked copy of the DDS extension,
    // while SetDdsTransportDriver() is called from the dynamically-loaded extension .so.
    dds_ext::DdsTransportDriver* driver = nullptr;
    for (auto* td : core.GetTransportBus().GetUsedTransportDriver()) {
      if (td->Name() == "dds") {
        driver = dynamic_cast<dds_ext::DdsTransportDriver*>(td);
        break;
      }
    }
    if (driver) {
      plain_pub.SetDdsDriver(driver);
      nonplain_pub.SetDdsDriver(driver);
      std::cout << "DDS Driver found - LoanedMessage API enabled\n";
    } else {
      std::cout << "DDS Driver not found\n";
    }

    std::cout << "Press Ctrl+C to exit\n";
    std::cout << "========================================\n";

    core.Start();
    core.Shutdown();

    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return -1;
  }

  return 0;
}
