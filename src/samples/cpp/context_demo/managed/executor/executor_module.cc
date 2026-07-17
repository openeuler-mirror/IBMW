// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "executor/executor_module.h"

#include <chrono>
#include <thread>

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
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  MW_INFO("Stop succeeded.");
}

void ExecutorModule::TimeScheduleDemo() {
  const auto count = ++loop_count_;
  MW_INFO("Timer loop count : {}", count);

  time_schedule_executor_.DispatchAfter(
      std::chrono::seconds(1),
      [this]() {
        ctx_ptr_->LetMe();
        if (!ibmw::context::Running()) {
          return;
        }
        time_schedule_executor_.Dispatch([this] { TimeScheduleDemo(); });
      });
}

}  // namespace ibmw::examples::cpp::context::executor_module
