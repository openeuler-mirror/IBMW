// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#include "executor_module/executor_module.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::executor::executor_module {
bool ExecutorModule::Setup(ibmw::Runtime core) {
  core_ = core;
  work_executor_ = core_.GetExecutor().GetExecutor("work_executor");
  MW_REQUIRE(work_executor_, "Can not get work_executor");
  thread_safe_executor_ = core_.GetExecutor().GetExecutor("thread_safe_executor");
  MW_REQUIRE(thread_safe_executor_ && thread_safe_executor_.ThreadSafe(), "Can not get thread_safe_executor");
  time_schedule_executor_ = core_.GetExecutor().GetExecutor("time_schedule_executor");
  MW_REQUIRE(time_schedule_executor_ && time_schedule_executor_.SupportsTimedDispatch(), "Can not get time_schedule_executor");
  MW_INFO("Setup succeeded.");
  return true;
}
bool ExecutorModule::Run() {
  SimpleExecuteDemo();
  ThreadSafeDemo();
  TimeScheduleDemo();
  MW_INFO("Run succeeded.");
  return true;
}
void ExecutorModule::Stop() {
  run_flag_ = false;
  // Wait until the pending TimeScheduleDemo callback has run and seen
  // !run_flag_, so no task referencing this module is left queued. The chain
  // period is 1 s; the deadline only guards against a lost callback.
  constexpr auto kChainStopTimeout = std::chrono::seconds(3);
  std::unique_lock<std::mutex> lock(chain_mutex_);
  if (!chain_cv_.wait_for(lock, kChainStopTimeout, [this] { return !chain_pending_; })) {
    MW_ERROR("Time schedule chain did not stop within {} s.", kChainStopTimeout.count());
    return;
  }
  MW_INFO("Stop succeeded.");
}
void ExecutorModule::SimpleExecuteDemo() {
  work_executor_.Dispatch([this]() { MW_INFO("This is a simple task"); });
}
void ExecutorModule::ThreadSafeDemo() {
  uint32_t n = 0;
  for (uint32_t ii = 0; ii < 10000; ++ii) { thread_safe_executor_.Dispatch([&n]() { n++; }); }
  // The strand runs tasks in dispatch order, so once this latch-tracked
  // marker has run every n++ above has run too (and happens-before the read).
  ibmw::util::DynamicLatch n_latch;
  thread_safe_executor_.TryDispatch(n_latch, []() {});
  n_latch.CloseAndWait();
  MW_INFO("Value of n is {}", n);
  uint32_t m = 0;
  for (uint32_t ii = 0; ii < 10000; ++ii) { thread_safe_executor_.TryDispatch(latch_, [&m]() { m++; }); }
  latch_.CloseAndWait();
  MW_INFO("Value of m is {}", m);
}
void ExecutorModule::TimeScheduleDemo() {
  if (!run_flag_) {
    std::lock_guard<std::mutex> lock(chain_mutex_);
    chain_pending_ = false;
    chain_cv_.notify_all();
    return;
  }
  MW_INFO("Loop count : {}", loop_count_++);
  {
    std::lock_guard<std::mutex> lock(chain_mutex_);
    chain_pending_ = true;
  }
  time_schedule_executor_.DispatchAfter(std::chrono::seconds(1), std::bind(&ExecutorModule::TimeScheduleDemo, this));
}
}
