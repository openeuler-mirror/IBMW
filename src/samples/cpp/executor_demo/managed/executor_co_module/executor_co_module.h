// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once
#include <atomic>
#include "cpp/co/ibmw_context.h"
#include "cpp/co/async_scope.h"
#include "cpp/co/task.h"
#include "cpp/module_base.h"

namespace ibmw::examples::cpp::executor::executor_co_module {
class ExecutorCoModule : public ibmw::Module {
 public:
  ExecutorCoModule() = default;
  ~ExecutorCoModule() override = default;
  ModuleDescriptor Info() const override { return ModuleDescriptor{.module_name = "ExecutorCoModule"}; }
  bool Setup(ibmw::Runtime ibmw_ptr) override;
  bool Run() override;
  void Stop() override;
 private:
  auto GetLogger() { return core_.GetLogger(); }
  co::Task<void> SimpleExecuteDemo();
  co::Task<void> ThreadSafeDemo();
  co::Task<void> TimeScheduleDemo();
 private:
  ibmw::Runtime core_;
  co::IbmwContext ctx_;
  co::AsyncScope scope_;
  std::atomic_bool run_flag_ = true;
};
}
