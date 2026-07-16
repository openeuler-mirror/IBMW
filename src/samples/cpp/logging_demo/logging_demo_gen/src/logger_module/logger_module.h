// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <future>

#include "cpp/module_base.h"

namespace logging_demo {

class LoggerModule : public ibmw::Module {
 public:
  LoggerModule() = default;
  ~LoggerModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "LoggerModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
  ibmw::executor::ExecutorHandle executor_;
  std::atomic_bool run_flag_{false};
  std::promise<void> stop_promise_;
};

}  // namespace logging_demo
