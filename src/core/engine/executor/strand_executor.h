// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "engine/executor/executor_base.h"
#include "base/log_util.h"

#include "yaml-cpp/yaml.h"

#include "asio.hpp"

namespace ibmw::runtime::core::executor {

class StrandExecutor : public ExecutorBase {
 public:
  struct Options {
    std::string bind_thread_pool_executor_name;
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

  using GetAsioHandle = std::function<asio::io_context*(std::string_view)>;

 public:
  StrandExecutor()
      : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}
  ~StrandExecutor() override = default;

  void Initialize(std::string_view name, YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  std::string_view Type() const noexcept override { return "strand"; }
  std::string_view Name() const noexcept override { return name_; }

  bool ThreadSafe() const noexcept override { return true; }
  bool IsInCurrentExecutor() const noexcept override { return false; }
  bool SupportsTimedDispatch() const noexcept override { return true; }

  void Dispatch(ibmw::executor::Task&& task) noexcept override;

  std::chrono::system_clock::time_point Now() const noexcept override;
  void DispatchAt(std::chrono::system_clock::time_point tp, ibmw::executor::Task&& task) noexcept override;

  void RegisterGetAsioHandle(GetAsioHandle&& handle);

  State GetState() const { return state_.load(); }

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

 private:
  std::string name_;
  Options options_;
  std::atomic<State> state_ = State::kPreInit;
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  std::chrono::system_clock::time_point start_sys_tp_;
  std::chrono::steady_clock::time_point start_std_tp_;

  GetAsioHandle get_asio_handle_;

  using Strand = asio::strand<asio::io_context::executor_type>;
  std::unique_ptr<Strand> strand_ptr_;
};

}  // namespace ibmw::runtime::core::executor
