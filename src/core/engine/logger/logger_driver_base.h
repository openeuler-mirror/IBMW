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
