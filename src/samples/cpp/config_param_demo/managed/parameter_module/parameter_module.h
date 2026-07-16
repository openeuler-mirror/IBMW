// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <future>

#include "cpp/module_base.h"

namespace ibmw::examples::cpp::parameter::parameter_module {

class ParameterModule : public ibmw::Module {
 public:
  ParameterModule() = default;
  ~ParameterModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "ParameterModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override;

  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  void SetParameterLoop();
  void GetParameterLoop();

 private:
  ibmw::Runtime core_;
  ibmw::executor::ExecutorHandle work_executor_;
  ibmw::parameter::ParamsRef parameter_handle_;

  std::atomic_bool run_flag_ = true;
  std::promise<void> set_loop_stop_sig_;
  std::promise<void> get_loop_stop_sig_;
};

}  // namespace ibmw::examples::cpp::parameter::parameter_module
