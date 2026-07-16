// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#include "executor_co_module/executor_co_module.h"
#include "cpp/co/ibmw_context.h"
#include "cpp/co/inline_executor.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::executor::executor_co_module {
bool ExecutorCoModule::Setup(ibmw::Runtime core) {
  core_ = core;
  ctx_ = co::IbmwContext(core_.GetExecutor());
  auto work_executor = core_.GetExecutor().GetExecutor("work_executor");
  MW_REQUIRE(work_executor, "Can not get work_executor");
  auto thread_safe_executor = core_.GetExecutor().GetExecutor("thread_safe_executor");
  MW_REQUIRE(thread_safe_executor && thread_safe_executor.ThreadSafe(), "Can not get thread_safe_executor");
  auto time_schedule_executor = core_.GetExecutor().GetExecutor("time_schedule_executor");
  MW_REQUIRE(time_schedule_executor && time_schedule_executor.SupportsTimedDispatch(), "Can not get time_schedule_executor");
  MW_INFO("Setup succeeded.");
  return true;
}
bool ExecutorCoModule::Run() {
  scope_.spawn(co::On(co::InlineExecutor(), SimpleExecuteDemo()));
  scope_.spawn(co::On(co::InlineExecutor(), ThreadSafeDemo()));
  scope_.spawn(co::On(co::InlineExecutor(), TimeScheduleDemo()));
  MW_INFO("Run succeeded.");
  return true;
}
void ExecutorCoModule::Stop() {
  run_flag_ = false;
  co::SyncWait(scope_.complete());
  MW_INFO("Stop succeeded.");
}
co::Task<void> ExecutorCoModule::SimpleExecuteDemo() {
  auto work_executor = ctx_.GetExecutor("work_executor");
  co_await co::Schedule(work_executor);
  MW_INFO("This is a simple task");
  co_return;
}
co::Task<void> ExecutorCoModule::ThreadSafeDemo() {
  auto thread_safe_executor = ctx_.GetExecutor("thread_safe_executor");
  co::AsyncScope scope;
  uint32_t n = 0;
  auto task = [&n]() -> co::Task<void> { n++; co_return; };
  for (size_t ii = 0; ii < 10000; ++ii) { scope.spawn(co::On(thread_safe_executor, task())); }
  co_await co::On(co::InlineExecutor(), scope.complete());
  MW_INFO("Value of n is {}", n);
  co_return;
}
co::Task<void> ExecutorCoModule::TimeScheduleDemo() {
  MW_INFO("Start loop.");
  auto time_executor = ctx_.GetExecutor("time_schedule_executor");
  co_await co::Schedule(time_executor);
  uint32_t count = 0;
  while (run_flag_) {
    count++;
    MW_INFO("Loop count : {} -------------------------", count);
    co_await co::ScheduleAfter(time_executor, std::chrono::seconds(1));
  }
  MW_INFO("Exit loop.");
  co_return;
}
}
