// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once
#include <atomic>
#include "cpp/co/ibmw_context.h"
#include "cpp/co/async_scope.h"
#include "cpp/co/task.h"
#include "cpp/module_base.h"

namespace ibmw::examples::cpp::executor::executor_co_loop_module {
class ExecutorCoLoopModule : public ibmw::Module {
 public:
  ExecutorCoLoopModule() = default;
  ~ExecutorCoLoopModule() override = default;
  ModuleDescriptor Info() const override { return ModuleDescriptor{.module_name = "ExecutorCoLoopModule"}; }
  bool Setup(ibmw::Runtime ibmw_ptr) override;
  bool Run() override;
  void Stop() override;
 private:
  auto GetLogger() { return core_.GetLogger(); }
  co::Task<void> MainLoop();
 private:
  ibmw::Runtime core_;
  ibmw::executor::ExecutorHandle time_schedule_executor_;
  co::AsyncScope scope_;
  std::atomic_bool run_flag_ = true;
};
}
