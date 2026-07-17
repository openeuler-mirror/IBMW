// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/module_base.h"

namespace executor_demo {

class TimerModule : public ibmw::Module {
 public:
  TimerModule() = default;
  ~TimerModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "TimerModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  // Required by MW_INFO/MW_WARN/... macros (they expand to GetLogger())
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
};

}  // namespace executor_demo
