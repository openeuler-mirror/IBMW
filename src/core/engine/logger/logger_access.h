// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include "c/logger/logger_base.h"
#include "engine/logger/logger_driver_base.h"

#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
  #include <sys/syscall.h>
  #define gettid() syscall(SYS_gettid)
#endif

namespace ibmw::runtime::core::logger {

class LoggerAccess {
 public:
  LoggerAccess(
      std::string_view module_name,
      ibmw_log_level_t lvl,
      const std::vector<std::unique_ptr<LoggerDriverBase>>& logger_driver_vec)
      : module_name_(module_name),
        lvl_(lvl),
        logger_driver_vec_(logger_driver_vec),
        base_(GenBase(this)) {}

  ~LoggerAccess() = default;

  LoggerAccess(const LoggerAccess&) = delete;
  LoggerAccess& operator=(const LoggerAccess&) = delete;

  const ibmw_logger_base_t* NativeHandle() const { return &base_; }

  // Atomic to avoid data races when level is modified at runtime
  ibmw_log_level_t LogLevel() const { return lvl_.load(std::memory_order_relaxed); }
  void SetLogLevel(ibmw_log_level_t lvl) { lvl_.store(lvl, std::memory_order_relaxed); }

 private:
  ibmw_log_level_t GetLogLevel() const { return lvl_.load(std::memory_order_relaxed); }

  void Log(ibmw_log_level_t lvl,
           uint32_t line,
           const char* file_name,
           const char* function_name,
           const char* log_data,
           size_t log_data_size) const {
    // Note: MW_LOG macro already checks lvl >= GetLogLevel() before calling
    // this method, so this check is redundant on the macro path. We keep it
    // as a safety net for direct C-ABI callers (cost: one relaxed atomic load).
    // Levels from OFF up are not message levels; the drivers index per-level
    // tables with lvl, so they are dropped here (CORE-038).
    if (lvl >= lvl_.load(std::memory_order_relaxed) &&
        static_cast<uint32_t>(lvl) < IBMW_LOG_LEVEL_OFF) {
#if defined(__GLIBC__)
      thread_local size_t tid(gettid());
#else
      thread_local size_t tid(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif

      LogDataWrapper log_data_wrapper{
          .module_name = module_name_,
          .thread_id = tid,
          .t = std::chrono::system_clock::now(),
          .lvl = lvl,
          .line = line,
          .file_name = file_name,
          .function_name = function_name,
          .log_data = log_data,
          .log_data_size = log_data_size};

      for (const auto& logger_driver_ptr : logger_driver_vec_) {
        logger_driver_ptr->Log(log_data_wrapper);
      }
    }
  }

  static ibmw_logger_base_t GenBase(void* ctx) {
    return ibmw_logger_base_t{
        .struct_size = sizeof(ibmw_logger_base_t),
        .get_log_level = [](void* ctx) -> ibmw_log_level_t {
          return static_cast<LoggerAccess*>(ctx)->GetLogLevel();
        },
        .log = [](void* ctx,
                  ibmw_log_level_t lvl,
                  uint32_t line,
                  const char* file_name,
                  const char* function_name,
                  const char* log_data,
                  size_t log_data_size) {
          static_cast<LoggerAccess*>(ctx)->Log(
              lvl,
              line,
              file_name,
              function_name,
              log_data,
              log_data_size);  //
        },
        .ctx = ctx};
  }

 private:
  const std::string module_name_;
  std::atomic<ibmw_log_level_t> lvl_;
  const std::vector<std::unique_ptr<LoggerDriverBase>>& logger_driver_vec_;

  const ibmw_logger_base_t base_;
};

}  // namespace ibmw::runtime::core::logger
