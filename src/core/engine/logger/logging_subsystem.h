// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

#include "cpp/executor/executor.h"
#include "engine/isubsystem.h"
#include "engine/logger/crash_signal_handling.h"
#include "engine/logger/logger_access.h"
#include "engine/core_types.h"
#include "base/string_util.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::logger {

class LoggingSubsystem : public ISubsystem {
 public:
  struct Options {
    ibmw_log_level_t core_lvl = ibmw_log_level_t::IBMW_LOG_LEVEL_INFO;
    ibmw_log_level_t default_module_lvl = ibmw_log_level_t::IBMW_LOG_LEVEL_INFO;
    bool enable_crash_log = false;

    struct DriverOptions {
      std::string type;
      YAML::Node options;
    };
    std::vector<DriverOptions> drivers_options;
  };

  using LoggerDriverGenFunc = std::function<std::unique_ptr<LoggerDriverBase>()>;

 public:
  LoggingSubsystem() = default;
  ~LoggingSubsystem() override;

  LoggingSubsystem(const LoggingSubsystem&) = delete;
  LoggingSubsystem& operator=(const LoggingSubsystem&) = delete;

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  /**
   * @brief Flush all logger drivers synchronously.
   * @note  Should be called before resetting the core logger to drain queued entries.
   */
  void Flush();

  /**
   * @brief Bounded flush for the crash report (see LoggerDriverBase::FlushForCrash).
   * @note  Waits at most `timeout` in total for all drivers. Called by the
   *        crash signal handler after it logged the report.
   * @return true when every driver wrote out what was logged before the call.
   */
  bool FlushForCrash(std::chrono::milliseconds timeout) noexcept;

  // Upper bound of the wait in FlushForCrash() when called by the crash
  // handler. Must stay well below the selfdev crash handler's report watchdog
  // (Options::report_timeout_sec, 10 s), so a log executor that is stuck
  // still lets the report finish and the process end with its signal.
  static constexpr std::chrono::milliseconds kCrashFlushTimeout{500};

  void RegisterGetExecutorFunc(
      const std::function<ibmw::executor::ExecutorHandle(std::string_view)>& get_executor_func);

  void RegisterLoggerDriverGenFunc(
      std::string_view type,
      LoggerDriverGenFunc&& logger_driver_gen_func);

  const LoggerAccess& GetLoggerAccess(const util::ModuleDetailInfo& module_info);
  const LoggerAccess& GetLoggerAccess(std::string_view logger_name = "core");

  std::unordered_map<std::string, ibmw_log_level_t> GetAllLoggerLevels() const;
  void SetLoggerLevels(const std::unordered_map<std::string, ibmw_log_level_t>& logger_lvls);

  std::list<std::pair<std::string, std::string>> GenInitializationReport() const override;

  const std::vector<std::unique_ptr<LoggerDriverBase>>& GetUsedLoggerDriver() const;

 private:
  void RegisterConsoleLoggerDriverGenFunc();
  void RegisterRotateFileLoggerDriverGenFunc();

 private:
  Options options_;

  std::unique_ptr<CrashSignalHandling> crash_signal_handling_;

  std::function<ibmw::executor::ExecutorHandle(std::string_view)> get_executor_func_;

  std::unordered_map<std::string, LoggerDriverGenFunc> logger_driver_gen_func_map_;

  std::vector<std::unique_ptr<LoggerDriverBase>> logger_driver_vec_;

  std::unordered_map<
      std::string,
      std::unique_ptr<LoggerAccess>,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      logger_access_map_;
};

}  // namespace ibmw::runtime::core::logger
