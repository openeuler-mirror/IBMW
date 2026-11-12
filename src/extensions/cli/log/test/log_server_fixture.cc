// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Independent runtime using the real log extension and module-facing logger API.
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
    ibmw::logger::LoggerRef car, other;
    std::atomic_bool ready = false;
    engine.RegisterHook(core::After(core::Phase::kInitialize, core::Subsystem::kLogger), [&] {
      auto& logging = engine.GetLoggingSubsystem();
      car = ibmw::logger::LoggerRef(logging.GetLoggerAccess(core::util::ModuleDetailInfo{
          .module_name = "CarModule", .use_default_log_lvl = true}).NativeHandle());
      other = ibmw::logger::LoggerRef(logging.GetLoggerAccess(core::util::ModuleDetailInfo{
          .module_name = "OtherModule", .use_default_log_lvl = true}).NativeHandle());
      logging.GetLoggerAccess("中文模块");
      logging.GetLoggerAccess(std::string(2048, 'x'));
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
    while (!stopped) {
      std::string token;
      std::getline(std::ifstream(argv[2]), token);
      if (!token.empty() && token != previous) {
        // Console uses one sequential executor. The final marker is submitted
        // after the tested messages, allowing the test to await a complete batch.
        for (unsigned level = 0; level < IBMW_LOG_LEVEL_OFF; ++level) {
          const auto message = token + ":" + std::to_string(level);
          car.Log(level, __LINE__, __FILE__, __func__, message.data(), message.size());
        }
        const auto end = token + ":END";
        other.Log(IBMW_LOG_LEVEL_FATAL, __LINE__, __FILE__, __func__, end.data(), end.size());
        previous = token;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    engine.Shutdown();
    running.get();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
