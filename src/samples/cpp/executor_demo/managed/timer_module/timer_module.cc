// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#include "timer_module/timer_module.h"
#include <chrono>
#include <memory>

namespace ibmw::examples::cpp::executor::timer_module {
bool TimerModule::Setup(ibmw::Runtime core) {
  core_ = core;
  timer_executor_ = core_.GetExecutor().GetExecutor("timer_executor");
  MW_REQUIRE(timer_executor_, "Can not get timer_executor");
  MW_REQUIRE(timer_executor_.SupportsTimedDispatch(), "timer_executor does not support timer schedule");
  return true;
}
bool TimerModule::Run() {
  using namespace std::chrono_literals;
  auto start_time = timer_executor_.Now();
  auto task = [logger = core_.GetLogger(), start_time](ibmw::executor::TimerBase& timer) {
    static int count = 0;
    auto now = timer.Executor().Now();
    auto timepoint = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
    MW_LOG(logger, ibmw::common::util::kLogLevelInfo, "Executed {} times, execute timepoint: {} ms", ++count, timepoint);
    if (count >= 10) {
      timer.Cancel();
      MW_LOG(logger, ibmw::common::util::kLogLevelInfo, "Timer cancelled at timepoint: {} ms", timepoint);
    }
  };
  timer_ = ibmw::executor::CreateTimer(timer_executor_, 100ms, std::move(task));
  MW_INFO("Timer created at timepoint: 0 ms");
  timer_executor_.DispatchAfter(350ms, [this, logger = core_.GetLogger()]() {
    timer_->Reset();
    MW_LOG(logger, ibmw::common::util::kLogLevelInfo, "Timer reset at timepoint: 350 ms");
  });
  timer_executor_.DispatchAfter(600ms, [this, logger = core_.GetLogger()]() {
    timer_->Reset();
    MW_LOG(logger, ibmw::common::util::kLogLevelInfo, "Timer reset at timepoint: 600 ms");
  });
  return true;
}
void TimerModule::Stop() { timer_->Cancel(); }
}
