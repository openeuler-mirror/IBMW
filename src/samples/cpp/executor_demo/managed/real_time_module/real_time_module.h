// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once
#include <atomic>
#include "cpp/co/async_scope.h"
#include "cpp/co/task.h"
#include "cpp/module_base.h"

namespace ibmw::examples::cpp::executor::real_time_module {
class RealTimeModule : public ibmw::Module {
 public:
  RealTimeModule() = default;
  ~RealTimeModule() override = default;
  ModuleDescriptor Info() const override { return ModuleDescriptor{.module_name = "RealTimeModule"}; }
  bool Setup(ibmw::Runtime ibmw_ptr) override;
  bool Run() override;
  void Stop() override;
 private:
  auto GetLogger() { return core_.GetLogger(); }
  void StartWorkLoopByExecutor(std::string_view executor_name);
  co::Task<void> WorkLoop(ibmw::executor::ExecutorHandle executor_ptr);
 private:
  ibmw::Runtime core_;
  co::AsyncScope scope_;
  std::atomic_bool run_flag_ = true;
};
}
