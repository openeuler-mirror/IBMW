// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Independent process exercising the real parameter extension and module-facing API.
#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>
#include "engine/engine.h"

namespace {
volatile std::sig_atomic_t stopped = 0;
void Stop(int) { stopped = 1; }
}

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    namespace core = ibmw::runtime::core;
    core::Engine engine;
    ibmw::parameter::ParamsRef car;
    std::atomic_bool ready = false;
    engine.RegisterHook(core::After(core::Phase::kInitialize, core::Subsystem::kParameter), [&] {
      car = ibmw::parameter::ParamsRef(engine.GetParameterStore().GetParamsAccess("CarModule").NativeHandle());
      car.SetParameter("speed", "0.2");
      car.SetParameter("controller.gain", "2");
      auto other = ibmw::parameter::ParamsRef(engine.GetParameterStore().GetParamsAccess("OtherModule").NativeHandle());
      other.SetParameter("speed", "7");
    });
    engine.RegisterHook(core::AfterPhase(core::Phase::kStart), [&] { ready = true; });
    engine.Initialize({.cfg_file_path = argv[1]});
    std::signal(SIGINT, Stop);
    std::signal(SIGTERM, Stop);
    auto running = engine.AsyncStart();
    while (!ready && running.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {}
    if (!ready) { running.get(); throw std::runtime_error("Runtime did not start."); }
    std::cout << "READY" << std::endl;
    std::string previous;
    bool first = true;
    while (!stopped) {
      auto value = car.GetParameter("speed");
      if (first || value != previous) {
        std::ofstream(argv[2], std::ios::trunc) << value;
        previous = std::move(value);
        first = false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    engine.Shutdown();
    running.get();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
