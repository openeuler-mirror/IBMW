// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Diagnostics sink of the public DDS-only action layers. These layers run
// without a framework logger, so they write to stdout/stderr by default;
// SetActionLogSink() lets the application redirect or silence the output.

#ifndef IBMW_API_CPP_ACTION_ACTION_LOG_H_
#define IBMW_API_CPP_ACTION_ACTION_LOG_H_

#include <atomic>
#include <iostream>
#include <string>
#include <string_view>

namespace ibmw::action {

enum class ActionLogLevel { kInfo, kWarn, kError };

// Called from DDS listener and worker threads, so it must be thread safe.
using ActionLogSink = void (*)(ActionLogLevel level, std::string_view message) noexcept;

namespace detail {

inline std::atomic<ActionLogSink> g_action_log_sink{nullptr};

inline void WriteActionLogToStdStreams(ActionLogLevel level, std::string_view message) {
  const std::string_view tag = level == ActionLogLevel::kInfo   ? "[INFO] "
                               : level == ActionLogLevel::kWarn ? "[WARN] "
                                                                : "[ERROR] ";
  std::string line;
  line.reserve(tag.size() + message.size() + 1);
  line.append(tag).append(message).push_back('\n');
  auto& stream = level == ActionLogLevel::kInfo ? std::cout : std::cerr;
  stream << line << std::flush;
}

}  // namespace detail

// Passing nullptr restores the default stdout/stderr output.
inline void SetActionLogSink(ActionLogSink sink) noexcept {
  detail::g_action_log_sink.store(sink, std::memory_order_release);
}

inline void ActionLog(ActionLogLevel level, std::string_view message) {
  if (const auto sink = detail::g_action_log_sink.load(std::memory_order_acquire)) {
    sink(level, message);
    return;
  }
  detail::WriteActionLogToStdStreams(level, message);
}

}  // namespace ibmw::action

#endif  // IBMW_API_CPP_ACTION_ACTION_LOG_H_
