// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <memory>
#include <vector>

#include "extension_api/ibmw_discovery_provider.h"

namespace ibmw::discovery {

// Returns nullptr when the selected backend plugin is not installed.
std::unique_ptr<DiscoveryProvider> CreateDiscoveryProvider(
    DiscoveryBackend backend);
DiscoverySnapshot Discover(
    const std::vector<DiscoveryOptions>& sources,
    std::chrono::milliseconds timeout = std::chrono::seconds(2));

}  // namespace ibmw::discovery
