// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/executor/main_loop_executor.h"
#include "engine/thread_tools.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::executor::MainLoopExecutor::Options> {
  using Options = ibmw::runtime::core::executor::MainLoopExecutor::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["name"] = rhs.name;
    node["thread_sched_policy"] = rhs.thread_sched_policy;
    node["thread_bind_cpu"] = rhs.thread_bind_cpu;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["name"])
      rhs.name = node["name"].as<std::string>();

    if (node["thread_sched_policy"])
      rhs.thread_sched_policy = node["thread_sched_policy"].as<std::string>();

    if (node["thread_bind_cpu"])
      rhs.thread_bind_cpu = node["thread_bind_cpu"].as<std::vector<uint32_t>>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::executor {

void MainLoopExecutor::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Main thread executor can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  name_ = options_.name;

  try {
    util::SetNameForCurrentThread(name_);
    util::BindCpuForCurrentThread(options_.thread_bind_cpu);
    util::SetCpuSchedForCurrentThread(options_.thread_sched_policy);
  } catch (const std::exception& e) {
    MW_WARN("Set thread policy for main thread get exception, {}", e.what());
  }

  main_thread_id_ = std::this_thread::get_id();

  options_node = options_;

  MW_INFO("[MainLoop] initialized");
}

void MainLoopExecutor::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Main thread executor can only run when state is 'Init'.");
}

void MainLoopExecutor::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  MW_INFO("[MainLoop] stopped");
}

}  // namespace ibmw::runtime::core::executor
