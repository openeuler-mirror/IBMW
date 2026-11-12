// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>

#include "cli/command.h"
#include "extension_api/ibmw_service_discovery.h"

namespace ibmw::cli::service {

void AddDiscoveryOptions(Command& command);
std::filesystem::path ParseRegistryPath(const Invocation& invocation);
std::vector<ibmw::discovery::ServiceDiscoveryOptions> ParseDiscoveryOptions(const Invocation& invocation);
std::chrono::milliseconds ParseDiscoveryTimeout(const Invocation& invocation);

}  // namespace ibmw::cli::service
