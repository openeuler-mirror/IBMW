// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <chrono>
#include <string>
#include <vector>
#include "cli/command.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::cli::service {
struct CallOptions {
  std::string backend, service, type, request, serialization;
  std::vector<std::string> packages;
  std::chrono::milliseconds timeout{5000}, discovery_timeout{2000};
  YAML::Node root;
};
void AddCallOptions(Command& command);
void AddCallTransportOptions(Command& command, bool native_ros2 = true);
CallOptions ParseCallOptions(const Invocation& invocation);
std::string CallLibrary(std::string_view filename);
}  // namespace ibmw::cli::service
