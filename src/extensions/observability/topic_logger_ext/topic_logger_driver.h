// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <mutex>
#include <queue>
#include <shared_mutex>
#include <unordered_map>

#include "cpp/transport/transport_handle.h"
#include "cpp/executor/executor.h"
#include "cpp/executor/timer.h"
#include "engine/logger/logger_driver_base.h"
#include "topic_logger_ext/topic_logger_types.h"
#include "base/string_util.h"

namespace ibmw::extensions::topic_logger_extension {

class TopicLoggerDriver : public runtime::core::logger::LoggerDriverBase {
 public:
  struct Options {
    std::string module_filter = "(.*)";  // default: match all modules
    uint32_t interval_ms = 500;          // default: 500ms
    std::string timer_executor_name;
    std::string topic_name;
    size_t max_msg_size = SIZE_MAX;
    size_t max_msg_count = SIZE_MAX;
  };

 public:
  TopicLoggerDriver() = default;
  ~TopicLoggerDriver() override = default;

  std::string_view Type() const noexcept override { return "topic_logger"; }

  void Initialize(YAML::Node options_node) override;
  void Start() override {}
  void Shutdown() override {
    run_flag_.store(false);
  }

  void RegisterGetExecutorFunc(
      const std::function<ibmw::executor::ExecutorHandle(std::string_view)>& get_executor_func) {
    get_executor_func_ = get_executor_func;
  }

  void RegisterGetSenderRefFunc(
      const std::function<ibmw::transport::SenderRef(std::string_view)>& get_publisher_ref_func) {
    get_publisher_ref_func_ = get_publisher_ref_func;
  }

  void RegisterLogPublisher();

  void SetMaxMsgSize() { max_msg_size_ = options_.max_msg_size; }

  void StartupPulisher() {
    publish_flag_ = true;
    timer_ptr->Reset();
  }

  void StopPulisher() {
    run_flag_.store(false);
    publish_flag_ = false;
    timer_ptr->Cancel();
  }

  bool AllowDuplicates() const noexcept override { return true; }

  void Log(const runtime::core::logger::LogDataWrapper& log_data_wrapper) noexcept override;

 private:
  bool CheckLog(const runtime::core::logger::LogDataWrapper& log_data_wrapper);

 private:
  Options options_;

  std::function<ibmw::executor::ExecutorHandle(std::string_view)> get_executor_func_;
  ibmw::executor::ExecutorHandle timer_executor_;
  std::shared_ptr<ibmw::executor::TimerBase> timer_ptr;

  std::function<ibmw::transport::SenderRef(std::string_view)> get_publisher_ref_func_;
  ibmw::transport::SenderRef log_publisher_;

  std::atomic_bool run_flag_ = false;
  bool publish_flag_ = false;

  std::shared_mutex module_filter_map_mutex_;
  std::unordered_map<
      std::string, bool, ibmw::common::util::StringHash, std::equal_to<>>
      module_filter_map_;

  std::mutex mutex_;
  std::unique_ptr<ibmw::protocols::topic_logger::LogData> current_log_data_;

  size_t max_msg_size_ = SIZE_MAX;
  uint64_t sequence_num_{0};
  uint64_t dropped_msg_count_{0};
};

}  // namespace ibmw::extensions::topic_logger_extension
