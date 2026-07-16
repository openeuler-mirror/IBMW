// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "c/logger/logger_base.h"
#include "base/log_util.h"

namespace ibmw::logger {

class LoggerRef {
 public:
  LoggerRef() = default;
  explicit LoggerRef(const ibmw_logger_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~LoggerRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_logger_base_t* NativeHandle() const { return base_ptr_; }

  uint32_t GetLogLevel() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return static_cast<uint32_t>(base_ptr_->get_log_level(base_ptr_->ctx));
  }

  void Log(
      uint32_t lvl,
      uint32_t line,
      const char* file_name,
      const char* function_name,
      const char* log_data,
      size_t log_data_size) const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->log(
        base_ptr_->ctx,
        static_cast<ibmw_log_level_t>(lvl),
        line, file_name, function_name,
        log_data, log_data_size);
  };

 private:
  const ibmw_logger_base_t* base_ptr_ = nullptr;
};

inline LoggerRef GetSimpleLoggerRef() {
  static constexpr ibmw_logger_base_t kSimpleLogger{
      .get_log_level = [](void* ctx) -> ibmw_log_level_t {
        return static_cast<ibmw_log_level_t>(ibmw::common::util::SimpleLogger::GetLogLevel());
      },
      .log = [](void* ctx, ibmw_log_level_t lvl, uint32_t line,
                const char* file_name, const char* function_name,
                const char* log_data, size_t log_data_size) {
        ibmw::common::util::SimpleLogger::Log(
            static_cast<uint32_t>(lvl), line, file_name, function_name, log_data, log_data_size);  //
      }};

  return LoggerRef(&kSimpleLogger);
}

}  // namespace ibmw::logger
