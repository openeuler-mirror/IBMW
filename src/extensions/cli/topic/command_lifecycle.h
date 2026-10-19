// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once
#include <csignal>
#include <stdexcept>
#include "base/log_util.h"
namespace ibmw::cli::topic {
inline volatile std::sig_atomic_t stop_requested = 0;
inline void RequestStop(int) { stop_requested = 1; }

class SignalGuard {
 public:
  SignalGuard() {
    stop_requested = 0;
    struct sigaction action {};
    action.sa_handler = RequestStop;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGINT, &action, &interrupt_) != 0) {
      throw std::runtime_error("Cannot install SIGINT handler.");
    }
    if (::sigaction(SIGTERM, &action, &terminate_) != 0) {
      ::sigaction(SIGINT, &interrupt_, nullptr);
      throw std::runtime_error("Cannot install SIGTERM handler.");
    }
  }
  ~SignalGuard() {
    ::sigaction(SIGINT, &interrupt_, nullptr);
    ::sigaction(SIGTERM, &terminate_, nullptr);
  }
 private:
  struct sigaction interrupt_ {}, terminate_ {};
};

class LoggerLevelGuard {
 public:
  LoggerLevelGuard() : previous_(ibmw::common::util::SimpleLogger::GetLogLevel()) {
    ibmw::common::util::SimpleLogger::SetLogLevel(ibmw::common::util::kLogLevelWarn);
  }
  ~LoggerLevelGuard() { ibmw::common::util::SimpleLogger::SetLogLevel(previous_); }
 private:
  uint32_t previous_;
};

}  // namespace ibmw::cli::topic
