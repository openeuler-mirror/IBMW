// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once

#include <stdexcept>
#include "cli/command.h"

namespace ibmw::cli::topic {
inline std::string OptionValue(const Invocation& invocation, const std::string& name,
                               std::string fallback = {}) {
  const auto it = invocation.options.find(name);
  if (it == invocation.options.end()) return fallback;
  if (it->second.size() != 1 || it->second.front().empty()) {
    throw std::invalid_argument(name + " requires exactly one nonempty value.");
  }
  return it->second.front();
}

}  // namespace ibmw::cli::topic
