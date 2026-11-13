// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

#include "cpp/executor/executor.h"
#include "cpp/util/string.h"
#include "engine/isubsystem.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::executor {

class WatchdogExecutor : public ISubsystem {
 public:
  struct Options {
    std::string name = "ibmw_guard";
    std::string thread_sched_policy;
    std::vector<uint32_t> thread_bind_cpu;
    uint32_t queue_threshold = 10000;
    int32_t threshold_alarm_interval_ms = 1000;
  };

 public:
  WatchdogExecutor()
      : base_(GenBase(this)) {}
  ~WatchdogExecutor() override = default;

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  std::string_view Type() const { return "watchdog"; }
  std::string_view Name() const { return name_; }

  bool ThreadSafe() const { return true; }
  bool IsInCurrentExecutor() const {
    return std::this_thread::get_id() == thread_id_.load(std::memory_order_relaxed);
  }
  bool SupportsTimedDispatch() const { return false; }

  void Dispatch(ibmw::executor::Task&& task);

  size_t CurrentTaskNum() noexcept { return queue_task_num_.load(); }

  const ibmw_executor_base_t* NativeHandle() const { return &base_; }

 private:
  static ibmw_executor_base_t GenBase(void* ctx) {
    return ibmw_executor_base_t{
        .struct_size = sizeof(ibmw_executor_base_t),
        .type = [](void* ctx) -> ibmw_string_view_t {
          return ibmw::util::ToIbmwStringView(
              static_cast<WatchdogExecutor*>(ctx)->Type());
        },
        .name = [](void* ctx) -> ibmw_string_view_t {
          return ibmw::util::ToIbmwStringView(
              static_cast<WatchdogExecutor*>(ctx)->Name());
        },
        .is_thread_safe = [](void* ctx) -> bool {
          return static_cast<WatchdogExecutor*>(ctx)->ThreadSafe();
        },
        .is_in_current_executor = [](void* ctx) -> bool {
          return static_cast<WatchdogExecutor*>(ctx)->IsInCurrentExecutor();
        },
        .supports_timed_dispatch = [](void* ctx) -> bool {
          return static_cast<WatchdogExecutor*>(ctx)->SupportsTimedDispatch();
        },
        .dispatch = [](void* ctx, ibmw_function_base_t* task) {
          static_cast<WatchdogExecutor*>(ctx)->Dispatch(ibmw::executor::Task(task));  //
        },
        .now = nullptr,
        .dispatch_at_ns = nullptr,
        .ctx = ctx};
  }

 private:
  Options options_;

  ibmw::common::util::SimpleLogger simple_logger_;  // deal with log printed in Dispatch()

  std::string name_;
  std::atomic<std::thread::id> thread_id_{};

  uint32_t queue_threshold_;
  uint32_t queue_warn_threshold_;
  std::atomic_uint32_t queue_task_num_ = 0;

  std::mutex mutex_;
  std::condition_variable cond_;
  std::queue<ibmw::executor::Task> queue_;
  std::unique_ptr<std::thread> thread_ptr_;

  const ibmw_executor_base_t base_;
};

}  // namespace ibmw::runtime::core::executor
