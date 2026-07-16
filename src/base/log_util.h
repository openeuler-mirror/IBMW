// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include "base/block_queue.h"
#include "base/exception.h"
#include "base/format.h"
#include "base/source_location.h"
#include "base/time_util.h"

#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
  #include <sys/syscall.h>
  #define gettid() syscall(SYS_gettid)
#endif

namespace ibmw::common::util {

constexpr uint32_t kLogLevelTrace = 0;
constexpr uint32_t kLogLevelDebug = 1;
constexpr uint32_t kLogLevelInfo = 2;
constexpr uint32_t kLogLevelWarn = 3;
constexpr uint32_t kLogLevelError = 4;
constexpr uint32_t kLogLevelFatal = 5;

class SimpleLogger {
 public:
  static uint32_t GetLogLevel() { return 0; }

  static void Log(uint32_t lvl,
                  uint32_t line,
                  const char* file_name,
                  const char* function_name,
                  const char* log_data,
                  size_t log_data_size) {
    static constexpr std::string_view kLvlNameArray[] = {
        "Trace", "Debug", "Info", "Warn", "Error", "Fatal"};

    static constexpr uint32_t kLvlNameArraySize =
        sizeof(kLvlNameArray) / sizeof(kLvlNameArray[0]);
    lvl = lvl >= kLvlNameArraySize ? (kLvlNameArraySize - 1) : lvl;

    auto t = std::chrono::system_clock::now();
    // Extract short file name (strip directory path)
    const char* short_name = file_name;
    if (file_name) {
      const char* p = std::strrchr(file_name, '/');
      if (p) short_name = p + 1;
    }

    std::string log_str = ::ibmw_fmt::format(
        "{} {}.{:0>6} {}  ({}:{})",
        kLvlNameArray[lvl],
        GetTimeStr(std::chrono::system_clock::to_time_t(t)),
        (GetTimestampUs(t) % 1000000),
        std::string_view(log_data, log_data_size),
        short_name ? short_name : "?",
        line);

    fprintf(stderr, "%s\n", log_str.c_str());
  }
};

class SimpleAsyncLogger {
 public:
  SimpleAsyncLogger()
      : log_thread_(std::bind(&SimpleAsyncLogger::LogThread, this)) {}

  ~SimpleAsyncLogger() {
    queue_.Stop();
    if (log_thread_.joinable()) log_thread_.join();
  }

  SimpleAsyncLogger(const SimpleAsyncLogger&) = delete;
  SimpleAsyncLogger& operator=(const SimpleAsyncLogger&) = delete;

  static uint32_t GetLogLevel() { return 0; }

  void Log(uint32_t lvl,
           uint32_t line,
           const char* file_name,
           const char* function_name,
           const char* log_data,
           size_t log_data_size) const {
    static constexpr std::string_view kLvlNameArray[] = {
        "Trace", "Debug", "Info", "Warn", "Error", "Fatal"};

    static constexpr uint32_t kLvlNameArraySize =
        sizeof(kLvlNameArray) / sizeof(kLvlNameArray[0]);
    if (lvl >= kLvlNameArraySize) [[unlikely]]
      lvl = kLvlNameArraySize - 1;

    auto t = std::chrono::system_clock::now();

    // Extract short file name (strip directory path)
    const char* short_name = file_name;
    if (file_name) {
      const char* p = std::strrchr(file_name, '/');
      if (p) short_name = p + 1;
    }

    std::string log_str = ::ibmw_fmt::format(
        "{} {}.{:0>6} {}  ({}:{})",
        kLvlNameArray[lvl],
        GetTimeStr(std::chrono::system_clock::to_time_t(t)),
        (GetTimestampUs(t) % 1000000),
        std::string_view(log_data, log_data_size),
        short_name ? short_name : "?",
        line);
    try {
      queue_.Enqueue(std::move(log_str));
    } catch (...) {
    }
  }

 private:
  void LogThread() {
    while (true) {
      try {
        auto log_str = queue_.Dequeue();
        fprintf(stderr, "%s\n", log_str.c_str());
      } catch (...) {
        break;
      }
    }
  }

  mutable BlockQueue<std::string> queue_;
  std::thread log_thread_;
};

struct LoggerWrapper {
  uint32_t GetLogLevel() const {
    return get_log_level_func();
  }

  void Log(uint32_t lvl,
           uint32_t line,
           const char* file_name,
           const char* function_name,
           const char* log_data,
           size_t log_data_size) const {
    log_func(lvl, line, file_name, function_name, log_data, log_data_size);
  }

  using GetLogLevelFunc = std::function<uint32_t(void)>;
  using LogFunc = std::function<void(uint32_t, uint32_t, const char*, const char*, const char*, size_t)>;

  GetLogLevelFunc get_log_level_func = SimpleLogger::GetLogLevel;
  LogFunc log_func = SimpleLogger::Log;
};

template <typename T>
concept LoggerType =
    requires(T t) {
      { t.GetLogLevel() } -> std::same_as<uint32_t>;
      { t.Log(0, 0, nullptr, nullptr, nullptr, 0) };
    };

template <LoggerType Logger, typename... Args>
inline void LogImpl(const Logger& logger,
                    uint32_t lvl,
                    uint32_t line,
                    const char* file_name,
                    const char* function_name,
                    ::ibmw_fmt::format_string<Args...> fmt,
                    Args&&... args) {
  std::string log_str = ::ibmw_fmt::format(fmt, std::forward<Args>(args)...);
  logger.Log(lvl, line, file_name, function_name, log_str.c_str(), log_str.size());
}

}  // namespace ibmw::common::util

// ============================================================================
// IBMW Logging Macro System
// ============================================================================
//
// Architecture: 2-tier (core + convenience), level-as-parameter.
//
// Plugin logger binding:
//   Plugins define MW_LOGGER before including this header to override the
//   default logger. This eliminates the per-extension #undef/#define boilerplate.
//
// Core macros (explicit logger):
//   MW_LOG(logger, level, fmt, ...)
//   MW_LOG_STREAM(logger, level, stream_expr)
//   MW_LOG_ONCE(logger, level, fmt, ...)
//   MW_LOG_IF(cond, logger, level, fmt, ...)
//   MW_LOG_INTERVAL(ms, logger, level, fmt, ...)
//   MW_LOG_EVERY(count, logger, level, fmt, ...)
//   MW_CHECK_LOG(logger, expr, level, fmt, ...)
//   MW_LOG_THROW(logger, level, fmt, ...)
//   MW_CHECK_LOG_THROW(logger, expr, level, fmt, ...)
//
// Convenience macros (use MW_LOGGER):
//   MW_TRACE/MW_DEBUG/MW_INFO/MW_WARN/MW_ERROR/MW_FATAL(fmt, ...)
//   MW_REQUIRE(expr, fmt, ...)      -- check + log(error) + throw
//   MW_THROW(fmt, ...)              -- log(error) + throw
//   MW_WARN_IF(expr, fmt, ...)      -- check + log(warn)
//   MW_{LEVEL}_INTERVAL(ms, fmt, ...)
//   MW_{LEVEL}_ONCE(fmt, ...)
//   MW_{LEVEL}_STREAM(stream_expr)
//   MW_{LEVEL}_IF(cond, fmt, ...)
//   MW_{LEVEL}_EVERY(count, fmt, ...)
//   MW_{LEVEL}_THROW(fmt, ...)
//   MW_CHECK_{LEVEL}(expr, fmt, ...)
//   MW_CHECK_{LEVEL}_THROW(expr, fmt, ...)
//
// ============================================================================

// ---------------------------------------------------------------------------
// Core macros (explicit logger + level)
// ---------------------------------------------------------------------------

/// Log with explicit logger and level
#define MW_LOG(__lgr__, __lvl__, __fmt__, ...)                                  \
  do {                                                                          \
    const auto& __mw_lgr__ = __lgr__;                                           \
    if ((__lvl__) >= __mw_lgr__.GetLogLevel()) {                                \
      std::string __mw_log__ = ::ibmw_fmt::format(__fmt__, ##__VA_ARGS__);      \
      constexpr auto __mw_loc__ = ::ibmw::common::util::SourceLocation::current(); \
      __mw_lgr__.Log(                                                           \
          (__lvl__), __mw_loc__.line(), __mw_loc__.file_name(), __FUNCTION__,   \
          __mw_log__.c_str(), __mw_log__.size());                               \
    }                                                                           \
  } while (0)

/// Log a stream expression with explicit logger and level
#define MW_LOG_STREAM(__lgr__, __lvl__, __stream__)                             \
  do {                                                                          \
    const auto& __mw_lgr__ = (__lgr__);                                         \
    if ((__lvl__) >= __mw_lgr__.GetLogLevel()) {                                \
      std::stringstream __mw_ss__;                                              \
      __mw_ss__ << __stream__;                                                  \
      const std::string& __mw_log__ = __mw_ss__.str();                          \
      constexpr auto __mw_loc__ = ::ibmw::common::util::SourceLocation::current(); \
      __mw_lgr__.Log(                                                           \
          (__lvl__), __mw_loc__.line(), __mw_loc__.file_name(), __FUNCTION__,   \
          __mw_log__.c_str(), __mw_log__.size());                               \
    }                                                                           \
  } while (0)

/// Log once (first hit only)
#define MW_LOG_ONCE(__lgr__, __lvl__, __fmt__, ...)                             \
  do {                                                                          \
    static std::atomic<bool> __mw_once__{false};                                \
    if (!__mw_once__.load(std::memory_order_acquire)) {                         \
      bool __mw_expected__ = false;                                             \
      if (__mw_once__.compare_exchange_strong(                                  \
              __mw_expected__, true, std::memory_order_release)) {              \
        MW_LOG(__lgr__, __lvl__, __fmt__, ##__VA_ARGS__);                       \
      }                                                                         \
    }                                                                           \
  } while (0)

/// Log if condition is true
#define MW_LOG_IF(__cond__, __lgr__, __lvl__, __fmt__, ...)                     \
  do {                                                                          \
    if (__cond__) {                                                             \
      MW_LOG(__lgr__, __lvl__, __fmt__, ##__VA_ARGS__);                         \
    }                                                                           \
  } while (0)

/// Log at most once per N milliseconds
#define MW_LOG_INTERVAL(__ms__, __lgr__, __lvl__, __fmt__, ...)                 \
  do {                                                                          \
    static std::atomic<uint64_t> __mw_last_ms__{0};                             \
    auto __mw_now__ = std::chrono::duration_cast<std::chrono::milliseconds>(    \
                          std::chrono::steady_clock::now().time_since_epoch())   \
                          .count();                                             \
    auto __mw_prev__ = __mw_last_ms__.load(std::memory_order_relaxed);          \
    if (__mw_prev__ == 0 ||                                                     \
        (static_cast<uint64_t>(__mw_now__) - __mw_prev__) >=                    \
            static_cast<uint64_t>(__ms__)) {                                    \
      if (__mw_last_ms__.compare_exchange_strong(                               \
              __mw_prev__, static_cast<uint64_t>(__mw_now__),                   \
              std::memory_order_relaxed)) {                                     \
        MW_LOG(__lgr__, __lvl__, __fmt__, ##__VA_ARGS__);                       \
      }                                                                         \
    }                                                                           \
  } while (0)

/// Log once every N calls
#define MW_LOG_EVERY(__count__, __lgr__, __lvl__, __fmt__, ...)                 \
  do {                                                                          \
    static std::atomic<int> __mw_ctr__{1};                                      \
    if (__mw_ctr__.fetch_sub(1, std::memory_order_relaxed) == 1) {              \
      __mw_ctr__.store((__count__), std::memory_order_relaxed);                 \
      MW_LOG(__lgr__, __lvl__, __fmt__, ##__VA_ARGS__);                         \
    }                                                                           \
  } while (0)

/// Check expression; if false, log at given level (no throw)
#define MW_CHECK_LOG(__lgr__, __expr__, __lvl__, __fmt__, ...)                  \
  do {                                                                          \
    if (!(__expr__)) [[unlikely]] {                                             \
      MW_LOG(__lgr__, __lvl__, __fmt__, ##__VA_ARGS__);                         \
    }                                                                           \
  } while (0)

/// Log at given level and throw IbmwException
#define MW_LOG_THROW(__lgr__, __lvl__, __fmt__, ...)                            \
  do {                                                                          \
    std::string __mw_log__ = ::ibmw_fmt::format(__fmt__, ##__VA_ARGS__);        \
    const auto& __mw_lgr__ = __lgr__;                                           \
    if ((__lvl__) >= __mw_lgr__.GetLogLevel()) {                                \
      constexpr auto __mw_loc__ = ::ibmw::common::util::SourceLocation::current(); \
      __mw_lgr__.Log(                                                           \
          (__lvl__), __mw_loc__.line(), __mw_loc__.file_name(),                 \
          __FUNCTION__, __mw_log__.c_str(), __mw_log__.size());                 \
    }                                                                           \
    throw ibmw::common::util::IbmwException(std::move(__mw_log__));             \
  } while (0)

/// Check expression; if false, log at given level and throw
#define MW_CHECK_LOG_THROW(__lgr__, __expr__, __lvl__, __fmt__, ...)            \
  do {                                                                          \
    if (!(__expr__)) [[unlikely]] {                                             \
      MW_LOG_THROW(__lgr__, __lvl__, __fmt__, ##__VA_ARGS__);                   \
    }                                                                           \
  } while (0)

// ---------------------------------------------------------------------------
// Default logger binding
// ---------------------------------------------------------------------------
// Plugins override MW_LOGGER before including this header or in their global.h.
// The default resolves to GetLogger() which works in module/core contexts.

#ifndef MW_LOGGER
  #define MW_LOGGER GetLogger()
#endif

// ---------------------------------------------------------------------------
// Level convenience macros (use MW_LOGGER)
// ---------------------------------------------------------------------------

#define MW_TRACE(__fmt__, ...) MW_LOG(MW_LOGGER, ibmw::common::util::kLogLevelTrace, __fmt__, ##__VA_ARGS__)
#define MW_DEBUG(__fmt__, ...) MW_LOG(MW_LOGGER, ibmw::common::util::kLogLevelDebug, __fmt__, ##__VA_ARGS__)
#define MW_INFO(__fmt__, ...)  MW_LOG(MW_LOGGER, ibmw::common::util::kLogLevelInfo,  __fmt__, ##__VA_ARGS__)
#define MW_WARN(__fmt__, ...)  MW_LOG(MW_LOGGER, ibmw::common::util::kLogLevelWarn,  __fmt__, ##__VA_ARGS__)
#define MW_ERROR(__fmt__, ...) MW_LOG(MW_LOGGER, ibmw::common::util::kLogLevelError, __fmt__, ##__VA_ARGS__)
#define MW_FATAL(__fmt__, ...) MW_LOG(MW_LOGGER, ibmw::common::util::kLogLevelFatal, __fmt__, ##__VA_ARGS__)

// Stream variants
#define MW_TRACE_STREAM(__s__) MW_LOG_STREAM(MW_LOGGER, ibmw::common::util::kLogLevelTrace, __s__)
#define MW_DEBUG_STREAM(__s__) MW_LOG_STREAM(MW_LOGGER, ibmw::common::util::kLogLevelDebug, __s__)
#define MW_INFO_STREAM(__s__)  MW_LOG_STREAM(MW_LOGGER, ibmw::common::util::kLogLevelInfo,  __s__)
#define MW_WARN_STREAM(__s__)  MW_LOG_STREAM(MW_LOGGER, ibmw::common::util::kLogLevelWarn,  __s__)
#define MW_ERROR_STREAM(__s__) MW_LOG_STREAM(MW_LOGGER, ibmw::common::util::kLogLevelError, __s__)
#define MW_FATAL_STREAM(__s__) MW_LOG_STREAM(MW_LOGGER, ibmw::common::util::kLogLevelFatal, __s__)

// Once variants
#define MW_TRACE_ONCE(__fmt__, ...) MW_LOG_ONCE(MW_LOGGER, ibmw::common::util::kLogLevelTrace, __fmt__, ##__VA_ARGS__)
#define MW_DEBUG_ONCE(__fmt__, ...) MW_LOG_ONCE(MW_LOGGER, ibmw::common::util::kLogLevelDebug, __fmt__, ##__VA_ARGS__)
#define MW_INFO_ONCE(__fmt__, ...)  MW_LOG_ONCE(MW_LOGGER, ibmw::common::util::kLogLevelInfo,  __fmt__, ##__VA_ARGS__)
#define MW_WARN_ONCE(__fmt__, ...)  MW_LOG_ONCE(MW_LOGGER, ibmw::common::util::kLogLevelWarn,  __fmt__, ##__VA_ARGS__)
#define MW_ERROR_ONCE(__fmt__, ...) MW_LOG_ONCE(MW_LOGGER, ibmw::common::util::kLogLevelError, __fmt__, ##__VA_ARGS__)
#define MW_FATAL_ONCE(__fmt__, ...) MW_LOG_ONCE(MW_LOGGER, ibmw::common::util::kLogLevelFatal, __fmt__, ##__VA_ARGS__)

// Conditional variants
#define MW_TRACE_IF(__c__, __fmt__, ...) MW_LOG_IF(__c__, MW_LOGGER, ibmw::common::util::kLogLevelTrace, __fmt__, ##__VA_ARGS__)
#define MW_DEBUG_IF(__c__, __fmt__, ...) MW_LOG_IF(__c__, MW_LOGGER, ibmw::common::util::kLogLevelDebug, __fmt__, ##__VA_ARGS__)
#define MW_INFO_IF(__c__, __fmt__, ...)  MW_LOG_IF(__c__, MW_LOGGER, ibmw::common::util::kLogLevelInfo,  __fmt__, ##__VA_ARGS__)
#define MW_WARN_IF(__c__, __fmt__, ...)  MW_LOG_IF(__c__, MW_LOGGER, ibmw::common::util::kLogLevelWarn,  __fmt__, ##__VA_ARGS__)
#define MW_ERROR_IF(__c__, __fmt__, ...) MW_LOG_IF(__c__, MW_LOGGER, ibmw::common::util::kLogLevelError, __fmt__, ##__VA_ARGS__)
#define MW_FATAL_IF(__c__, __fmt__, ...) MW_LOG_IF(__c__, MW_LOGGER, ibmw::common::util::kLogLevelFatal, __fmt__, ##__VA_ARGS__)

// Interval variants
#define MW_TRACE_INTERVAL(__ms__, __fmt__, ...) MW_LOG_INTERVAL(__ms__, MW_LOGGER, ibmw::common::util::kLogLevelTrace, __fmt__, ##__VA_ARGS__)
#define MW_DEBUG_INTERVAL(__ms__, __fmt__, ...) MW_LOG_INTERVAL(__ms__, MW_LOGGER, ibmw::common::util::kLogLevelDebug, __fmt__, ##__VA_ARGS__)
#define MW_INFO_INTERVAL(__ms__, __fmt__, ...)  MW_LOG_INTERVAL(__ms__, MW_LOGGER, ibmw::common::util::kLogLevelInfo,  __fmt__, ##__VA_ARGS__)
#define MW_WARN_INTERVAL(__ms__, __fmt__, ...)  MW_LOG_INTERVAL(__ms__, MW_LOGGER, ibmw::common::util::kLogLevelWarn,  __fmt__, ##__VA_ARGS__)
#define MW_ERROR_INTERVAL(__ms__, __fmt__, ...) MW_LOG_INTERVAL(__ms__, MW_LOGGER, ibmw::common::util::kLogLevelError, __fmt__, ##__VA_ARGS__)
#define MW_FATAL_INTERVAL(__ms__, __fmt__, ...) MW_LOG_INTERVAL(__ms__, MW_LOGGER, ibmw::common::util::kLogLevelFatal, __fmt__, ##__VA_ARGS__)

// Every-N variants
#define MW_TRACE_EVERY(__n__, __fmt__, ...) MW_LOG_EVERY(__n__, MW_LOGGER, ibmw::common::util::kLogLevelTrace, __fmt__, ##__VA_ARGS__)
#define MW_DEBUG_EVERY(__n__, __fmt__, ...) MW_LOG_EVERY(__n__, MW_LOGGER, ibmw::common::util::kLogLevelDebug, __fmt__, ##__VA_ARGS__)
#define MW_INFO_EVERY(__n__, __fmt__, ...)  MW_LOG_EVERY(__n__, MW_LOGGER, ibmw::common::util::kLogLevelInfo,  __fmt__, ##__VA_ARGS__)
#define MW_WARN_EVERY(__n__, __fmt__, ...)  MW_LOG_EVERY(__n__, MW_LOGGER, ibmw::common::util::kLogLevelWarn,  __fmt__, ##__VA_ARGS__)
#define MW_ERROR_EVERY(__n__, __fmt__, ...) MW_LOG_EVERY(__n__, MW_LOGGER, ibmw::common::util::kLogLevelError, __fmt__, ##__VA_ARGS__)
#define MW_FATAL_EVERY(__n__, __fmt__, ...) MW_LOG_EVERY(__n__, MW_LOGGER, ibmw::common::util::kLogLevelFatal, __fmt__, ##__VA_ARGS__)

// Check (log on failure, no throw)
#define MW_CHECK_TRACE(__expr__, __fmt__, ...) MW_CHECK_LOG(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelTrace, __fmt__, ##__VA_ARGS__)
#define MW_CHECK_DEBUG(__expr__, __fmt__, ...) MW_CHECK_LOG(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelDebug, __fmt__, ##__VA_ARGS__)
#define MW_CHECK_INFO(__expr__, __fmt__, ...)  MW_CHECK_LOG(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelInfo,  __fmt__, ##__VA_ARGS__)
#define MW_CHECK_WARN(__expr__, __fmt__, ...)  MW_CHECK_LOG(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelWarn,  __fmt__, ##__VA_ARGS__)
#define MW_CHECK_ERROR(__expr__, __fmt__, ...) MW_CHECK_LOG(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelError, __fmt__, ##__VA_ARGS__)
#define MW_CHECK_FATAL(__expr__, __fmt__, ...) MW_CHECK_LOG(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelFatal, __fmt__, ##__VA_ARGS__)

// Throw (log + throw, unconditional)
#define MW_TRACE_THROW(__fmt__, ...) MW_LOG_THROW(MW_LOGGER, ibmw::common::util::kLogLevelTrace, __fmt__, ##__VA_ARGS__)
#define MW_DEBUG_THROW(__fmt__, ...) MW_LOG_THROW(MW_LOGGER, ibmw::common::util::kLogLevelDebug, __fmt__, ##__VA_ARGS__)
#define MW_INFO_THROW(__fmt__, ...)  MW_LOG_THROW(MW_LOGGER, ibmw::common::util::kLogLevelInfo,  __fmt__, ##__VA_ARGS__)
#define MW_WARN_THROW(__fmt__, ...)  MW_LOG_THROW(MW_LOGGER, ibmw::common::util::kLogLevelWarn,  __fmt__, ##__VA_ARGS__)
#define MW_ERROR_THROW(__fmt__, ...) MW_LOG_THROW(MW_LOGGER, ibmw::common::util::kLogLevelError, __fmt__, ##__VA_ARGS__)
#define MW_FATAL_THROW(__fmt__, ...) MW_LOG_THROW(MW_LOGGER, ibmw::common::util::kLogLevelFatal, __fmt__, ##__VA_ARGS__)

// Check + throw (log + throw on failure)
#define MW_CHECK_TRACE_THROW(__expr__, __fmt__, ...) MW_CHECK_LOG_THROW(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelTrace, __fmt__, ##__VA_ARGS__)
#define MW_CHECK_DEBUG_THROW(__expr__, __fmt__, ...) MW_CHECK_LOG_THROW(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelDebug, __fmt__, ##__VA_ARGS__)
#define MW_CHECK_INFO_THROW(__expr__, __fmt__, ...)  MW_CHECK_LOG_THROW(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelInfo,  __fmt__, ##__VA_ARGS__)
#define MW_CHECK_WARN_THROW(__expr__, __fmt__, ...)  MW_CHECK_LOG_THROW(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelWarn,  __fmt__, ##__VA_ARGS__)
#define MW_CHECK_ERROR_THROW(__expr__, __fmt__, ...) MW_CHECK_LOG_THROW(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelError, __fmt__, ##__VA_ARGS__)
#define MW_CHECK_FATAL_THROW(__expr__, __fmt__, ...) MW_CHECK_LOG_THROW(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelFatal, __fmt__, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Semantic aliases (the most common patterns get short names)
// ---------------------------------------------------------------------------

/// MW_REQUIRE(expr, fmt, ...) — check + log(error) + throw. The workhorse.
#define MW_REQUIRE(__expr__, __fmt__, ...) \
  MW_CHECK_LOG_THROW(MW_LOGGER, __expr__, ibmw::common::util::kLogLevelError, __fmt__, ##__VA_ARGS__)

/// MW_THROW(fmt, ...) — unconditional log(error) + throw.
#define MW_THROW(__fmt__, ...) \
  MW_LOG_THROW(MW_LOGGER, ibmw::common::util::kLogLevelError, __fmt__, ##__VA_ARGS__)
