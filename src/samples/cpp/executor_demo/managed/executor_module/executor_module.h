// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include "cpp/module_base.h"

namespace ibmw::examples::cpp::executor::executor_module {
class ExecutorModule : public ibmw::Module {
 public:
  ExecutorModule() = default;
  ~ExecutorModule() override = default;
  ModuleDescriptor Info() const override { return ModuleDescriptor{.module_name = "ExecutorModule"}; }
  bool Setup(ibmw::Runtime ibmw_ptr) override;
  bool Run() override;
  void Stop() override;
 private:
  auto GetLogger() { return core_.GetLogger(); }
  void SimpleExecuteDemo();
  void ThreadSafeDemo();
  void TimeScheduleDemo();
 private:
  ibmw::Runtime core_;
  ibmw::util::DynamicLatch latch_;
  ibmw::executor::ExecutorHandle work_executor_;
  ibmw::executor::ExecutorHandle thread_safe_executor_;
  std::atomic_bool run_flag_ = true;
  uint32_t loop_count_ = 0;
  // True while a TimeScheduleDemo re-dispatch is pending. Stop() waits for
  // the chain to observe !run_flag_ and clear it instead of sleeping.
  std::mutex chain_mutex_;
  std::condition_variable chain_cv_;
  bool chain_pending_ = false;
  ibmw::executor::ExecutorHandle time_schedule_executor_;
};
}
