// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "extension_api/ibmw_service_discovery.h"

namespace ibmw::discovery {

ServiceDiscoverySnapshot DiscoverRegistryServices(
    const std::filesystem::path& registry_path);

ServiceDiscoverySnapshot DiscoverServices(
    const std::vector<ServiceDiscoveryOptions>& sources,
    std::chrono::milliseconds timeout = std::chrono::seconds(2));

}  // namespace ibmw::discovery
