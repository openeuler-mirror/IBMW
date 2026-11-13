// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <thread>

#include "base/ibmw_config.h"
#include "engine/executor/executor_base.h"
#if defined(IBMW_SELFDEV_CONCURRENT)
  #include "ibmw_selfdev/concurrent/bounded_blocking_queue.h"
#else
  #include "tbb/concurrent_queue.h"
#endif
#include "base/log_util.h"

namespace ibmw::runtime::core::executor {

class ParallelExecutor : public ExecutorBase {
 public:
  struct Options {
    uint32_t thread_num = 1;
    std::string thread_sched_policy;
    std::vector<uint32_t> thread_bind_cpu;
    uint32_t queue_threshold = 10000;
    int32_t threshold_alarm_interval_ms = 1000;
  };

  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

 public:
  ParallelExecutor()
      : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}
  ~ParallelExecutor() override = default;

  void Initialize(std::string_view name, YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  std::string_view Type() const noexcept override { return "parallel"; }
  std::string_view Name() const noexcept override { return name_; }

  bool ThreadSafe() const noexcept override { return (options_.thread_num == 1); }
  bool IsInCurrentExecutor() const noexcept override;
  bool SupportsTimedDispatch() const noexcept override { return false; }

  void Dispatch(ibmw::executor::Task&& task) noexcept override;

  std::chrono::system_clock::time_point Now() const noexcept override;
  void DispatchAt(std::chrono::system_clock::time_point tp, ibmw::executor::Task&& task) noexcept override;

  size_t CurrentTaskNum() noexcept override { return queue_task_num_.load(); }

  State GetState() const { return state_.load(); }

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

 private:
  std::string name_;
  Options options_;
  std::atomic<State> state_ = State::kPreInit;
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  uint32_t queue_threshold_;
  uint32_t queue_warn_threshold_;
  std::atomic_uint32_t queue_task_num_ = 0;

#if defined(IBMW_SELFDEV_CONCURRENT)
  ibmw::selfdev::concurrent::BoundedBlockingQueue<ibmw::executor::Task> qu_;
#else
  tbb::concurrent_bounded_queue<ibmw::executor::Task> qu_;
#endif
  std::atomic_uint32_t work_thread_num_ = 0;

  static thread_local const ParallelExecutor* tl_current_executor_;
  std::list<std::thread> threads_;
};

}  // namespace ibmw::runtime::core::executor
