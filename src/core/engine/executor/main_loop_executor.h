// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "engine/isubsystem.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::executor {

class MainLoopExecutor : public ISubsystem {
 public:
  struct Options {
    std::string name = "ibmw_main";
    std::string thread_sched_policy;
    std::vector<uint32_t> thread_bind_cpu;
  };

 public:
  MainLoopExecutor() = default;
  ~MainLoopExecutor() override = default;

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  std::string_view Name() const { return name_; }

 private:
  Options options_;

  std::string name_;
  std::thread::id main_thread_id_;
};

}  // namespace ibmw::runtime::core::executor
