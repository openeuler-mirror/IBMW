// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once
#include <atomic>
#include <future>
#include "cpp/module_base.h"

namespace ibmw::examples::cpp::logger::logger_module {
class LoggerModule : public ibmw::Module {
 public:
  LoggerModule() = default;
  ~LoggerModule() override = default;
  ModuleDescriptor Info() const override { return ModuleDescriptor{.module_name = "LoggerModule"}; }
  bool Setup(ibmw::Runtime ibmw_ptr) override;
  bool Run() override;
  void Stop() override;
 private:
  ibmw::Runtime core_;
  ibmw::executor::ExecutorHandle executor_;
  std::atomic_bool run_flag_ = false;
  std::promise<void> stop_sig_;
};
}
