// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <map>
#include <set>
#include <string>

#include "cli/command.h"
#include "engine/discovery/service_discovery.h"

namespace ibmw::cli::service {

ibmw::discovery::ServiceDiscoverySnapshot DiscoverServices(const Invocation& invocation);

// Full method name -> distinct request/response type pairs; server endpoints only.
using ServiceTypes = std::map<std::string, std::set<std::string>>;
ServiceTypes VisibleServices(
    const ibmw::discovery::ServiceDiscoverySnapshot& snapshot, bool include_hidden);

}  // namespace ibmw::cli::service
