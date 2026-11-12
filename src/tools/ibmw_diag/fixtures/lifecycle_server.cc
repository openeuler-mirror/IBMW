// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Keep a real Runtime Agent serving after a real ModuleBus callback failure.
// No fabricated lifecycle response: the handler returns the bus's recorded state.
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>
#include "engine/module/module_bus.h"
#include "engine/runtime_graph/runtime_graph_agent.h"

namespace {
volatile std::sig_atomic_t stopped = 0;
void Stop(int) { stopped = 1; }
struct FailingModule {
  std::string mode;
  std::string journal;
  ibmw_module_base_t base{
      .info = [](void*) -> ibmw_module_info_t {
        return {.module_name = ibmw::util::ToIbmwStringView("FailingModule")};
      },
      .setup = [](void* ctx, const ibmw_core_base_t*) {
        auto& self = *static_cast<FailingModule*>(ctx);
        std::ofstream(self.journal, std::ios::app) << "setup\n";
        if (self.mode == "initialize") throw std::runtime_error("device initialization failed");
        return true;
      },
      .run = [](void* ctx) {
        auto& self = *static_cast<FailingModule*>(ctx);
        std::ofstream(self.journal, std::ios::app) << "run\n";
        if (self.mode == "start") throw std::runtime_error("device start failed");
        return true;
      },
      .stop = [](void*) {}, .ctx = this};
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  try {
    namespace core = ibmw::runtime::core;
    FailingModule module{argv[2], argv[3]};
    core::module::ModuleBus bus;
    bus.RegisterCoreAccessConfigurator([](const auto&, auto&) {});
    bus.RegisterModule("failure-fixture", &module.base);
    bool failed = false;
    try { bus.Initialize(YAML::Node{}); bus.Start(); }
    catch (const std::exception&) { failed = true; }
    if (!failed && module.mode != "unsupported") throw std::runtime_error("Failure injection did not run.");
    core::runtime_graph::RuntimeGraphAgent agent({
        .registration = {.runtime_id = "failure", .modules = std::vector<std::string>{"FailingModule"}},
        .registry_path = argv[1]});
    if (module.mode != "unsupported")
      agent.SetListModuleLifecycleHandler([&] { return bus.GetModuleLifecycleStatuses(); });
    agent.Initialize();
    agent.Start();
    std::signal(SIGINT, Stop);
    std::signal(SIGTERM, Stop);
    std::cout << "READY" << std::endl;
    while (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    agent.Shutdown();
    bus.Shutdown();
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
