// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "discovery_options.h"

#include <stdexcept>
#include <string>

#include "extension_api/ibmw_runtime_directory.h"

namespace ibmw::cli::service {

void AddDiscoveryOptions(Command& command) {
  command.AddOption("--registry", {}, {}, "Query local runtime registrations (default)");
  command.AddOption("--registry-path", {}, "path",
                    "Local registry root (default: " +
                        extension::DefaultRuntimeRegistryPath().string() + ")",
                    OptionValueMode::kRequired);
  command.AddOption("--include-hidden-services", {}, {}, "Consider hidden services as well");
}

std::filesystem::path ParseRegistryPath(const Invocation& invocation) {
  const auto found = invocation.options.find("--registry-path");
  if (found == invocation.options.end()) return extension::DefaultRuntimeRegistryPath();
  if (found->second.size() != 1 || found->second.front().empty()) {
    throw std::invalid_argument("--registry-path requires exactly one nonempty value.");
  }
  return found->second.front();
}

}  // namespace ibmw::cli::service
