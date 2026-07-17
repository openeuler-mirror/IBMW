// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <utility>

#include "cpp/context/op_base.h"
#include "cpp/logger/logger.h"

namespace ibmw::context {

class OpLog : public OpBase {
 public:
  using OpBase::OpBase;
  OpLog(Context& ctx, ibmw::common::util::SourceLocation loc) noexcept : OpBase(ctx, loc) {}

  template <class... Args>
  void Trace(fmt::format_string<Args...> f, Args&&... a) {
    Log(IBMW_LOG_LEVEL_TRACE, f, std::forward<Args>(a)...);
  }
  template <class... Args>
  void Debug(fmt::format_string<Args...> f, Args&&... a) {
    Log(IBMW_LOG_LEVEL_DEBUG, f, std::forward<Args>(a)...);
  }
  template <class... Args>
  void Info(fmt::format_string<Args...> f, Args&&... a) {
    Log(IBMW_LOG_LEVEL_INFO, f, std::forward<Args>(a)...);
  }
  template <class... Args>
  void Warn(fmt::format_string<Args...> f, Args&&... a) {
    Log(IBMW_LOG_LEVEL_WARN, f, std::forward<Args>(a)...);
  }
  template <class... Args>
  void Error(fmt::format_string<Args...> f, Args&&... a) {
    Log(IBMW_LOG_LEVEL_ERROR, f, std::forward<Args>(a)...);
  }
  template <class... Args>
  void Fatal(fmt::format_string<Args...> f, Args&&... a) {
    Log(IBMW_LOG_LEVEL_FATAL, f, std::forward<Args>(a)...);
  }

 private:
  template <class... Args>
  void Log(std::uint32_t level, fmt::format_string<Args...> f, Args&&... args);
};

}  // namespace ibmw::context
