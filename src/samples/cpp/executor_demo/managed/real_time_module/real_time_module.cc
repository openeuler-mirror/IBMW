// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#include "real_time_module/real_time_module.h"
#include "cpp/co/ibmw_context.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::executor::real_time_module {
bool RealTimeModule::Setup(ibmw::Runtime core) {
  core_ = core;
  MW_INFO("Setup succeeded.");
  return true;
}
bool RealTimeModule::Run() {
  StartWorkLoopByExecutor("sched_fifo_thread");
  StartWorkLoopByExecutor("sched_other_thread");
  StartWorkLoopByExecutor("sched_rr_thread");
  MW_INFO("Run succeeded.");
  return true;
}
void RealTimeModule::Stop() {
  try {
    run_flag_ = false;
    co::SyncWait(scope_.complete());
  } catch (const std::exception& e) {
    MW_ERROR("Shutdown failed, {}", e.what());
    return;
  }
  MW_INFO("Stop succeeded.");
}
void RealTimeModule::StartWorkLoopByExecutor(std::string_view executor_name) {
  auto executor = core_.GetExecutor().GetExecutor(executor_name);
  MW_REQUIRE(executor && executor.SupportsTimedDispatch(), "Get executor '{}' failed.", executor_name);
  scope_.spawn(co::On(co::IbmwExecutor(executor), WorkLoop(executor)));
}
co::Task<void> RealTimeModule::WorkLoop(ibmw::executor::ExecutorHandle executor) {
  try {
    MW_INFO("Start WorkLoop in {}.", executor.Name());
#ifdef __linux__
    char thread_name[16];
    pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name));
    int policy = 0;
    struct sched_param param;
    pthread_getschedparam(pthread_self(), &policy, &param);
    MW_INFO("Executor name: {}, thread_name: {}, policy: {}, priority: {}", executor.Name(), thread_name, policy, param.sched_priority);
#endif
    uint32_t count = 0;
    while (run_flag_) {
      count++;
      auto start_tp = std::chrono::steady_clock::now();
      co_await co::ScheduleAfter(co::IbmwExecutor(executor), std::chrono::milliseconds(1000));
      auto end_tp = std::chrono::steady_clock::now();
#ifdef __linux__
      cpu_set_t cur_cpuset;
      CPU_ZERO(&cur_cpuset);
      auto pthread_getaffinity_np_ret = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cur_cpuset);
      MW_REQUIRE(pthread_getaffinity_np_ret == 0, "Call 'pthread_getaffinity_np' get error: {}", pthread_getaffinity_np_ret);
      uint32_t cpu_size = std::thread::hardware_concurrency();
      std::string cur_cpuset_str;
      for (int ii = 0; ii < cpu_size; ii++) {
        if (CPU_ISSET(ii, &cur_cpuset)) { cur_cpuset_str += (std::to_string(ii) + ", "); }
      }
      cur_cpuset_str = cur_cpuset_str.substr(0, cur_cpuset_str.size() - 2);
      unsigned int current_cpu = 0, current_node = 0;
      int getcpu_ret = getcpu(&current_cpu, &current_node);
      MW_REQUIRE(getcpu_ret == 0, "Call 'getcpu' get error: {}", getcpu_ret);
      MW_INFO("Loop count: {}, executor name: {}, sleep for {}, cpu: {}, node: {}, use cpu: '{}'", count, executor.Name(), end_tp - start_tp, current_cpu, current_node, cur_cpuset_str);
#else
      MW_INFO("Loop count: {}, executor name: {}, sleep for {}", count, executor.Name(), end_tp - start_tp);
#endif
    }
    MW_INFO("Exit WorkLoop.");
  } catch (const std::exception& e) {
    MW_ERROR("Exit WorkLoop with exception, {}", e.what());
  }
  co_return;
}
}
