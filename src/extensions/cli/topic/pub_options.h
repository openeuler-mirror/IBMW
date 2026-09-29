// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once

#include <cstdint>
#include <string>

#include "cli/command.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::cli::topic {
struct PublicationConfig {
  std::string driver;
  YAML::Node root;
  std::string topic;
  std::string type_name;
  double rate;
  double wait;
  double keep_alive;
  uint64_t count;
};

void AddPublicationOptions(Command& command);
PublicationConfig ParsePublicationOptions(const Invocation& invocation);
}  // namespace ibmw::cli::topic
