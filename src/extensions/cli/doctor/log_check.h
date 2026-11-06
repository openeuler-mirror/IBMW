// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "topic_check.h"

namespace ibmw::cli::doctor {
struct LogCheckResult {
  std::string module, actual, expected;
  CheckStatus status = CheckStatus::kSkipped;
  std::string message;
};

// Compare runtime thresholds with explicit requirements; no assumed default.
// Requested/expected loggers absent from the response remain UNKNOWN.
std::vector<LogCheckResult> CheckLogLevels(
    const std::map<std::string, std::string>& actual,
    const std::map<std::string, std::string>& expected,
    const std::vector<std::string>& requested = {});
}  // namespace ibmw::cli::doctor
