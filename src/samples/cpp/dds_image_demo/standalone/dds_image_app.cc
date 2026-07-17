// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// DDS Image Demo: App registration mode
//
// This example demonstrates publishing and subscribing to large DDS messages
// (image data with 1MB fixed buffer) using hand-written DDS types and
// FastCDR serialization. No fastddsgen code generation is required.

#include <csignal>
#include <iostream>

#include "engine/engine.h"
#include "image_publisher/image_publisher.h"
#include "image_subscriber/image_subscriber.h"

using namespace ibmw::runtime::core;

Engine* global_core_ptr = nullptr;

void SignalHandler(int sig) {
  if (global_core_ptr && (sig == SIGINT || sig == SIGTERM)) {
    global_core_ptr->Shutdown();
    return;
  }
  raise(sig);
}

int32_t main(int32_t argc, char** argv) {
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  std::cout << "ibmw DDS Image Demo start." << std::endl;

  try {
    Engine core;
    global_core_ptr = &core;

    // Register modules before Initialize (registration mode)
    ibmw::examples::dds_image_demo::ImagePublisherModule publisher_module;
    core.GetModuleBus().RegisterModule(publisher_module.NativeHandle());

    ibmw::examples::dds_image_demo::ImageSubscriberModule subscriber_module;
    core.GetModuleBus().RegisterModule(subscriber_module.NativeHandle());

    // Initialize
    Engine::Options options;
    if (argc > 1) options.cfg_file_path = argv[1];

    core.Initialize(options);

    // Start (sync — blocks until shutdown)
    core.Start();

    // Shutdown
    core.Shutdown();
    global_core_ptr = nullptr;

  } catch (const std::exception& e) {
    std::cerr << "ibmw DDS Image Demo failed: " << e.what() << std::endl;
    return -1;
  }

  std::cout << "ibmw DDS Image Demo exit." << std::endl;
  return 0;
}
