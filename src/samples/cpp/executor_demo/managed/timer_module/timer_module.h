// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once
#include "cpp/executor/executor.h"
#include "cpp/executor/timer.h"
#include "cpp/module_base.h"

namespace ibmw::examples::cpp::executor::timer_module {
class TimerModule : public ibmw::Module {
 public:
  TimerModule() = default;
  ~TimerModule() override = default;
  [[nodiscard]] ModuleDescriptor Info() const override { return ModuleDescriptor{.module_name = "TimerModule"}; }
  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;
 private:
  auto GetLogger() { return core_.GetLogger(); }
 private:
  ibmw::Runtime core_;
  ibmw::executor::ExecutorHandle timer_executor_;
  std::shared_ptr<ibmw::executor::TimerBase> timer_;
};
}
