// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <vector>

#include "cli/command.h"
#include "engine/discovery/discovery_provider.h"

namespace ibmw::cli::topic {

void AddDiscoveryOptions(ibmw::cli::Command& command, bool allow_all = true);
std::vector<ibmw::discovery::DiscoveryOptions> ParseDiscoveryOptions(
    const ibmw::cli::Invocation& invocation);

}  // namespace ibmw::cli::topic
