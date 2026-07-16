// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <fstream>
#include <regex>
#include <shared_mutex>
#include <unordered_map>
#include "cpp/executor/executor.h"
#include "cpp/executor/timer.h"
#include "engine/logger/formatter.h"
#include "engine/logger/logger_driver_base.h"
#include "engine/logger/os.h"
#include "base/string_util.h"

namespace ibmw::runtime::core::logger {

class RotateFileLoggerDriver : public LoggerDriverBase {
 public:
  struct Options {
    std::string path = "./log";
    std::string filename = "ibmw.log";
    uint32_t max_file_size_m = 16;
    uint32_t max_file_num = 100;
    std::string module_filter = "(.*)";
    std::string pattern;
    bool enable_sync = false;
    uint32_t sync_interval_ms = 30000;  // default: 30 s
    std::string sync_executor_name = "";
    bool suffix_with_timestamp = true;
  };

 public:
  RotateFileLoggerDriver() = default;
  ~RotateFileLoggerDriver() override;

  std::string_view Type() const noexcept override { return "rotate_file"; }

  void Initialize(YAML::Node options_node) override;
  void Start() override {}
  void Shutdown() override {
    run_flag_.store(false);

    if (sync_timer_) {
      sync_timer_->Cancel();
    }
  }

  void Flush() noexcept override {
    if (ofs_.is_open()) ofs_.flush();
  }

  void RegisterGetExecutorFunc(
      const std::function<ibmw::executor::ExecutorHandle(std::string_view)>& get_executor_func) {
    get_executor_func_ = get_executor_func;
  }

  bool AllowDuplicates() const noexcept override { return true; }

  void Log(const LogDataWrapper& log_data_wrapper) noexcept override;

 private:
  bool OpenNewFile();
  void CleanLogFile();
  uint32_t GetNextIndex();
  bool CheckLog(const LogDataWrapper& log_data_wrapper);
  void Rename();

 private:
  Options options_;
  std::function<ibmw::executor::ExecutorHandle(std::string_view)> get_executor_func_;
  executor::ExecutorHandle log_executor_;
  executor::ExecutorHandle timer_executor_;

  std::string base_file_name_;
  std::ofstream ofs_;

  std::atomic_bool run_flag_ = false;

  std::shared_mutex module_filter_map_mutex_;
  std::unordered_map<
      std::string, bool, ibmw::common::util::StringHash, std::equal_to<>>
      module_filter_map_;
  LogFormatter formatter_;
  std::string pattern_ = "%l %c.%f [%n] %v (%G:%R)";
  std::regex compiled_module_filter_;  // Pre-compiled from options_.module_filter

  std::shared_ptr<ibmw::executor::TimerBase> sync_timer_;

  FILE* file_ = nullptr;  // used for fsync()
};

}  // namespace ibmw::runtime::core::logger
