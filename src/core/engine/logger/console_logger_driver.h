// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <iostream>
#include <regex>
#include <shared_mutex>
#include <unordered_map>

#include "cpp/executor/executor.h"
#include "engine/logger/formatter.h"
#include "engine/logger/logger_driver_base.h"
#include "base/string_util.h"

namespace ibmw::runtime::core::logger {

class ConsoleLoggerDriver : public LoggerDriverBase {
 public:
  struct Options {
    bool print_color = true;
    std::string module_filter = "(.*)";
    std::string pattern;
  };

 public:
  ConsoleLoggerDriver() = default;
  ~ConsoleLoggerDriver() override = default;

  std::string_view Type() const noexcept override { return "console"; }

  void Initialize(YAML::Node options_node) override;
  void Start() override {}
  void Shutdown() override { run_flag_.store(false); }

  void Flush() noexcept override {
    std::cout.flush();
  }

  void RegisterGetExecutorFunc(
      const std::function<ibmw::executor::ExecutorHandle(std::string_view)>& get_executor_func) {
    get_executor_func_ = get_executor_func;
  }

  bool AllowDuplicates() const noexcept override { return false; }

  void Log(const LogDataWrapper& log_data_wrapper) noexcept override;

 private:
  bool CheckLog(const LogDataWrapper& log_data_wrapper);

 private:
  Options options_;
  std::function<ibmw::executor::ExecutorHandle(std::string_view)> get_executor_func_;
  ibmw::executor::ExecutorHandle log_executor_;
  std::atomic_bool run_flag_ = false;

  std::shared_mutex module_filter_map_mutex_;
  std::unordered_map<
      std::string, bool, ibmw::common::util::StringHash, std::equal_to<>>
      module_filter_map_;

  LogFormatter formatter_;
  std::string pattern_ = "%l %c.%f [%n] %v (%G:%R)";
  std::regex compiled_module_filter_;  // Pre-compiled from options_.module_filter
};

}  // namespace ibmw::runtime::core::logger
