// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "cpp/context/context.h"
#include "cpp/executor/executor.h"
#include "cpp/logger/logger.h"
#include "cpp/module_base.h"

namespace ibmw::examples::cpp::context::executor_module {

class ExecutorModule : public ibmw::Module {
 public:
  ExecutorModule() = default;
  ~ExecutorModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "ContextExecutorModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override;

  void Stop() override;

 private:
  void TimeScheduleDemo();

 private:
  std::shared_ptr<ibmw::context::Context> ctx_ptr_;

  ibmw::executor::ExecutorHandle time_schedule_executor_;

  std::atomic_uint32_t loop_count_{0};
};

}  // namespace ibmw::examples::cpp::context::executor_module
