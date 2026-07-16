// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <list>
#include <memory>
#include <string>
#include <thread>

#include "engine/executor/executor_base.h"
#include "base/log_util.h"

#include "yaml-cpp/yaml.h"

#include "asio.hpp"

namespace ibmw::runtime::core::executor {

class ThreadPoolExecutor : public ExecutorBase {
 public:
  struct Options {
    uint32_t thread_num = 1;
    std::string thread_sched_policy;
    std::vector<uint32_t> thread_bind_cpu;
    std::chrono::nanoseconds timeout_alarm_threshold_us = std::chrono::seconds(1);
    bool use_system_clock = false;
    int32_t timeout_alarm_interval_ms = 1000;
  };

  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

 public:
  ThreadPoolExecutor()
      : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}
  ~ThreadPoolExecutor() override = default;

  void Initialize(std::string_view name, YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  std::string_view Type() const noexcept override { return "thread_pool"; }
  std::string_view Name() const noexcept override { return name_; }

  bool ThreadSafe() const noexcept override { return (options_.thread_num == 1); }
  bool IsInCurrentExecutor() const noexcept override;
  bool SupportsTimedDispatch() const noexcept override { return true; }

  void Dispatch(ibmw::executor::Task&& task) noexcept override;

  std::chrono::system_clock::time_point Now() const noexcept override;
  void DispatchAt(std::chrono::system_clock::time_point tp, ibmw::executor::Task&& task) noexcept override;

  State GetState() const { return state_.load(); }

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

  asio::io_context* IOCTX() { return io_ptr_.get(); }

 private:
  std::string name_;
  Options options_;
  std::atomic<State> state_ = State::kPreInit;
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  std::chrono::system_clock::time_point start_sys_tp_;
  std::chrono::steady_clock::time_point start_std_tp_;

  std::unique_ptr<asio::io_context> io_ptr_;
  std::unique_ptr<
      asio::executor_work_guard<asio::io_context::executor_type>>
      work_guard_ptr_;

  static thread_local const ThreadPoolExecutor* tl_current_executor_;
  std::list<std::thread> threads_;
};

}  // namespace ibmw::runtime::core::executor
