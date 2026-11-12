// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "log_check.h"

#include <set>
#include <utility>

#include "log_control_ext/log_control_types.h"

namespace ibmw::cli::doctor {
std::vector<LogCheckResult> CheckLogLevels(
    const std::map<std::string, std::string>& actual,
    const std::map<std::string, std::string>& expected,
    const std::vector<std::string>& requested) {
  namespace protocol = ibmw::protocols::log_control_extension;
  const std::set<std::string> scope(requested.begin(), requested.end());
  auto names = scope;
  for (const auto& [name, value] : actual) names.insert(name);
  for (const auto& [name, value] : expected) names.insert(name);
  std::vector<LogCheckResult> results;
  for (const auto& name : names) {
    LogCheckResult result{.module = name};
    const auto found = actual.find(name);
    const auto wanted = expected.find(name);
    if (wanted != expected.end()) result.expected = wanted->second;
    if (found == actual.end()) {
      result.status = CheckStatus::kUnknown;
      result.message = "Requested or expected logger was not returned by the selected log service.";
    } else {
      result.actual = found->second;
      const auto level = protocol::NormalizeLogLevel(result.actual);
      if (name.empty() || !level) {
        result.status = CheckStatus::kUnknown;
        result.message = "The returned logger name or actual log level is invalid.";
      } else if (!scope.empty() && !scope.contains(name)) {
        result.status = CheckStatus::kUnknown;
        result.message = "The log service returned a logger outside the requested scope.";
      } else {
        result.actual = *level;
        if (wanted == expected.end()) {
          result.message = "Actual runtime threshold observed; comparison skipped because no expected level was supplied.";
        } else if (const auto baseline = protocol::NormalizeLogLevel(result.expected)) {
          result.expected = *baseline;
          result.status = result.actual == result.expected ? CheckStatus::kOk : CheckStatus::kWarning;
          result.message = result.status == CheckStatus::kOk
              ? "Actual log level matches the explicit expectation."
              : "Actual log level differs from the explicit expectation; confirm whether this is intentional debugging.";
        } else {
          result.status = CheckStatus::kUnknown;
          result.message = "The expected log level is invalid; comparison could not be completed.";
        }
      }
    }
    results.push_back(std::move(result));
  }
  if (results.empty()) results.push_back({.message = "The log service returned no registered loggers."});
  return results;
}
}  // namespace ibmw::cli::doctor
