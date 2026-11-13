// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "executor/executor_module.h"

#include <chrono>

#include "cpp/executor/executor.h"
#include "base/exception.h"

namespace ibmw::examples::cpp::context::executor_module {

bool ExecutorModule::Setup(ibmw::Runtime core) {
  ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
  ctx_ptr_->LetMe();
  time_schedule_executor_ = ctx_ptr_->CreateExecutor("time_schedule_executor");

  MW_REQUIRE(time_schedule_executor_ && time_schedule_executor_.SupportsTimedDispatch(),
                          "Can not get time_schedule_executor");

  MW_INFO("Setup succeeded.");
  return true;
}

bool ExecutorModule::Run() {
  TimeScheduleDemo();

  MW_INFO("Run succeeded.");
  return true;
}

void ExecutorModule::Stop() {
  // Make the pending timer callback see !Running() and end the chain, then
  // wait for it so no task referencing this module is left queued. The timer
  // period is 1 s; the deadline only guards against a lost callback.
  ctx_ptr_->StopRunning();
  constexpr auto kChainStopTimeout = std::chrono::seconds(3);
  std::unique_lock<std::mutex> lock(chain_mutex_);
  if (!chain_cv_.wait_for(lock, kChainStopTimeout, [this] { return !chain_pending_; })) {
    MW_ERROR("Timer chain did not stop within {} s.", kChainStopTimeout.count());
    return;
  }
  MW_INFO("Stop succeeded.");
}

void ExecutorModule::TimeScheduleDemo() {
  const auto count = ++loop_count_;
  MW_INFO("Timer loop count : {}", count);

  {
    std::lock_guard<std::mutex> lock(chain_mutex_);
    chain_pending_ = true;
  }
  time_schedule_executor_.DispatchAfter(
      std::chrono::seconds(1),
      [this]() {
        ctx_ptr_->LetMe();
        if (!ibmw::context::Running()) {
          std::lock_guard<std::mutex> lock(chain_mutex_);
          chain_pending_ = false;
          chain_cv_.notify_all();
          return;
        }
        time_schedule_executor_.Dispatch([this] { TimeScheduleDemo(); });
      });
}

}  // namespace ibmw::examples::cpp::context::executor_module
