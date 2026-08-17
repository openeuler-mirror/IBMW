// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <exception>
#include <iostream>

#include "engine/engine.h"
#include "wide_subscriber_module.h"

int main(int argc, char** argv) {
  try {
    ibmw::runtime::core::Engine engine;
    ibmw::samples::dds_xml_wide::SubscriberModule module(
        [&engine] { engine.Shutdown(); });
    engine.GetModuleBus().RegisterModule(module.NativeHandle());

    ibmw::runtime::core::Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    }
    engine.Initialize(options);
    engine.Start();
    engine.Shutdown();
    return module.Passed() ? 0 : 1;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    std::cout << "IBMW_XML_WIDE_RESULT role=subscriber status=FAIL topics=0 "
                 "service=FAIL"
              << std::endl;
    return 1;
  }
}
