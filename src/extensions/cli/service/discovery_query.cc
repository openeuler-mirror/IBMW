// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "discovery_query.h"

#include <iostream>

#include "discovery_options.h"

namespace ibmw::cli::service {

ibmw::discovery::ServiceDiscoverySnapshot DiscoverServices(const Invocation& invocation) {
  auto snapshot = ibmw::discovery::DiscoverRegistryServices(ParseRegistryPath(invocation));
  for (const auto& source : snapshot.sources) {
    if (source.status != ibmw::discovery::DiscoveryStatus::kReady) {
      std::cerr << "ibmw service: error: registry: " << source.message << '\n';
    }
  }
  return snapshot;
}

ServiceTypes VisibleServices(
    const ibmw::discovery::ServiceDiscoverySnapshot& snapshot, bool include_hidden) {
  ServiceTypes result;
  for (const auto& endpoint : snapshot.services) {
    const auto& service = endpoint.advertisement;
    if (!service.server || (!include_hidden && service.hidden)) continue;
    result[service.service_name].insert(service.request_type + " -> " + service.response_type);
  }
  return result;
}

}  // namespace ibmw::cli::service
