// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "grpc_ext/grpc/timeout.h"

#include <regex>

namespace ibmw::extensions::grpc_extension::grpc {

std::optional<std::chrono::nanoseconds> ParseTimeout(const std::string& timeout_str) {
  static const std::regex kTimeoutRegex(R"((\d+)([HMSmun]))");
  std::smatch match;
  if (std::regex_match(timeout_str, match, kTimeoutRegex)) {
    int64_t value = std::stoll(match[1].str());
    char unit = match[2].str()[0];

    switch (unit) {
      case 'H':
        return std::chrono::hours(value);
      case 'M':
        return std::chrono::minutes(value);
      case 'S':
        return std::chrono::seconds(value);
      case 'm':
        return std::chrono::milliseconds(value);
      case 'u':
        return std::chrono::microseconds(value);
      case 'n':
        return std::chrono::nanoseconds(value);
      default:
        return std::nullopt;
    }
  }

  return std::nullopt;
}

std::string FormatTimeout(std::chrono::nanoseconds timeout) {
  return std::to_string(timeout.count()) + "n";
}

}  // namespace ibmw::extensions::grpc_extension::grpc
