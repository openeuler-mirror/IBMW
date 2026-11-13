// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Macros that replace the per-extension global.h / global.cc boilerplate.
//
// In global.h:
//   #pragma once
//   #include "extension_api/extension_logger.h"
//   IBMW_DECLARE_EXTENSION_LOGGER(ibmw::extensions::my_extension)
//   #undef MW_LOGGER
//   #define MW_LOGGER ibmw::extensions::my_extension::g_logger.Get()
//   #include "base/log_util.h"
//
// In global.cc:
//   #include "<extension>/global.h"
//   IBMW_DEFINE_EXTENSION_LOGGER(ibmw::extensions::my_extension)

#pragma once

#include <atomic>

#include "cpp/logger/logger.h"

namespace ibmw {

class ExtensionLogger {
 public:
  ExtensionLogger()
      : fallback_base_{
            .struct_size = sizeof(ibmw_logger_base_t),
            .get_log_level = [](void* ctx) -> ibmw_log_level_t {
              return static_cast<ibmw_log_level_t>(
                  static_cast<ExtensionLogger*>(ctx)->fallback_level_.load(
                      std::memory_order_relaxed));
            },
            .log = [](void* ctx,
                      ibmw_log_level_t level,
                      uint32_t line,
                      const char* file_name,
                      const char* function_name,
                      const char* log_data,
                      size_t log_data_size) {
              auto* self = static_cast<ExtensionLogger*>(ctx);
              if (static_cast<uint32_t>(level) <
                  self->fallback_level_.load(std::memory_order_relaxed)) {
                return;
              }
              ibmw::common::util::SimpleLogger::Log(
                  static_cast<uint32_t>(level), line, file_name, function_name,
                  log_data, log_data_size);
            },
            .ctx = this} {}

  void Set(ibmw::logger::LoggerRef ref) {
    if (ref) {
      fallback_level_.store(ref.GetLogLevel(), std::memory_order_relaxed);
    }
    logger_.store(ref, std::memory_order_release);
  }

  ibmw::logger::LoggerRef Get() const {
    auto ref = logger_.load(std::memory_order_acquire);
    return ref ? ref : ibmw::logger::LoggerRef(&fallback_base_);
  }

 private:
  std::atomic<uint32_t> fallback_level_{
      ibmw::common::util::kLogLevelTrace};
  ibmw_logger_base_t fallback_base_;
  std::atomic<ibmw::logger::LoggerRef> logger_{};
};

}  // namespace ibmw

/// Declare the extension's global ExtensionLogger as extern in the given namespace.
#define IBMW_DECLARE_EXTENSION_LOGGER(ns) \
  namespace ns { extern ::ibmw::ExtensionLogger g_logger; }

/// Define (allocate storage for) the extension's global ExtensionLogger.
#define IBMW_DEFINE_EXTENSION_LOGGER(ns) \
  namespace ns { ::ibmw::ExtensionLogger g_logger; }
