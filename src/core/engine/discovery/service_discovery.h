// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "extension_api/ibmw_discovery.h"
#include "extension_api/ibmw_runtime_graph.h"

namespace ibmw::discovery {

struct DiscoveredService {
  DiscoverySource source;
  std::string runtime_id;
  ibmw::extension::ServiceAdvertisement advertisement;
};

// Separate from the topic snapshot: adding service discovery must not change
// the existing topic discovery plugin ABI or turn RPC endpoints into topics.
struct ServiceDiscoverySnapshot {
  std::vector<DiscoveredService> services;
  std::vector<DiscoverySourceStatus> sources;

  bool Succeeded() const noexcept {
    if (sources.empty()) return false;
    for (const auto& source : sources) {
      if (source.status != DiscoveryStatus::kReady) return false;
    }
    return true;
  }
};

ServiceDiscoverySnapshot DiscoverRegistryServices(
    const std::filesystem::path& registry_path);

}  // namespace ibmw::discovery
