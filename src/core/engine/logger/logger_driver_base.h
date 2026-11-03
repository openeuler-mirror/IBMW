// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <list>
#include <string>
#include <string_view>
#include <utility>

#include "c/logger/logger_base.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::logger {

// ---------------------------------------------------------------------------
// LogDataWrapper — payload passed to LoggerDriverBase::Log()
// ---------------------------------------------------------------------------
struct LogDataWrapper {
  std::string_view module_name;
  size_t thread_id;
  std::chrono::system_clock::time_point t;
  ibmw_log_level_t lvl;
  uint32_t line;
  const char* file_name;
  const char* function_name;
  const char* log_data;
  size_t log_data_size;
};

// ---------------------------------------------------------------------------
// LoggerDriverBase — abstract interface for logger backends
// ---------------------------------------------------------------------------
class LoggerDriverBase {
 public:
  LoggerDriverBase() = default;
  virtual ~LoggerDriverBase() = default;

  LoggerDriverBase(const LoggerDriverBase&) = delete;
  LoggerDriverBase& operator=(const LoggerDriverBase&) = delete;

  virtual std::string_view Type() const noexcept = 0;  // It should always return the same value

  // Implementations assign their effective options back to options_node.
  // YAML::Node shares storage with the caller's tree, so this fills in the
  // config that --dump_cfg writes out; it is not a dead store.
  virtual void Initialize(YAML::Node options_node) = 0;
  virtual void Start() = 0;
  virtual void Shutdown() = 0;

  /**
   * @brief Flush all pending log entries synchronously.
   * @note  Called before Shutdown() and before the core logger is reset,
   *        giving async drivers a chance to drain their queues.
   *        Default implementation is a no-op (suitable for sync drivers).
   */
  virtual void Flush() noexcept {}

  /**
   * @brief Write out the entries logged so far, for the crash report.
   * @note  Called by the crash signal handler after it logged the report and
   *        before the process ends, possibly on any thread (including the
   *        driver's own log executor). Must stop waiting for the log
   *        executor at `deadline` and must not wait on its own thread.
   *        Returns true when everything logged before the call has been
   *        written. The console and rotate_file drivers (FlushOnLogExecutor)
   *        poll every 1 ms, so they return up to about one poll interval
   *        after `deadline`; handing the flush task to the executor can
   *        block without bound on the executor's queue lock if the crash
   *        interrupted a thread holding it (the selfdev handler's report
   *        watchdog ends the process then; backward-cpp has none).
   *        Default: Flush() (suitable for sync drivers).
   */
  virtual bool FlushForCrash(std::chrono::steady_clock::time_point deadline) noexcept {
    (void)deadline;
    Flush();
    return true;
  }

  virtual std::list<std::pair<std::string, std::string>> GenInitializationReport() const noexcept { return {}; }

  virtual bool AllowDuplicates() const noexcept = 0;  // It should always return the same value

  /**
   * @brief Do the log
   * @note
   * 1. This method will only be called after 'Initialize' and before 'Shutdown'.
   * 2. Driver can define the actual behavior.
   *
   * @param log_data_wrapper Log data
   */
  virtual void Log(const LogDataWrapper& log_data_wrapper) noexcept = 0;
};

}  // namespace ibmw::runtime::core::logger
