// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#include "executor_co_loop_module/executor_co_loop_module.h"
#include "cpp/co/ibmw_context.h"
#include "cpp/co/inline_executor.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::executor::executor_co_loop_module {
bool ExecutorCoLoopModule::Setup(ibmw::Runtime core) {
  core_ = core;
  time_schedule_executor_ = core_.GetExecutor().GetExecutor("time_schedule_executor");
  MW_REQUIRE(time_schedule_executor_ && time_schedule_executor_.SupportsTimedDispatch(), "Can not get time_schedule_executor");
  MW_INFO("Setup succeeded.");
  return true;
}
bool ExecutorCoLoopModule::Run() {
  scope_.spawn(co::On(co::InlineExecutor(), MainLoop()));
  MW_INFO("Run succeeded.");
  return true;
}
void ExecutorCoLoopModule::Stop() {
  run_flag_ = false;
  co::SyncWait(scope_.complete());
  MW_INFO("Stop succeeded.");
}
co::Task<void> ExecutorCoLoopModule::MainLoop() {
  MW_INFO("Start loop.");
  auto executor = ibmw::co::IbmwExecutor(time_schedule_executor_);
  co_await co::Schedule(executor);
  uint32_t count = 0;
  while (run_flag_) {
    count++;
    MW_INFO("Loop count : {} -------------------------", count);
    co_await co::ScheduleAfter(executor, std::chrono::seconds(1));
  }
  MW_INFO("Exit loop.");
  co_return;
}
}
