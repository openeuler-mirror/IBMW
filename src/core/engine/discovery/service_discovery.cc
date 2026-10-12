// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/discovery/service_discovery.h"

#include <exception>
#include <stdexcept>

#include "engine/runtime_graph/runtime_registry.h"

namespace ibmw::discovery {

ServiceDiscoverySnapshot DiscoverRegistryServices(
    const std::filesystem::path& registry_path) {
  const DiscoverySource source{
      .backend = DiscoveryBackend::kRegistry,
      .instance_id = registry_path.lexically_normal().string()};
  ServiceDiscoverySnapshot result;
  DiscoverySourceStatus status{.source = source, .status = DiscoveryStatus::kReady};
  try {
    if (registry_path.empty()) throw std::invalid_argument("Registry path must not be empty.");
    if (std::filesystem::exists(registry_path) && !std::filesystem::is_directory(registry_path)) {
      throw std::invalid_argument("Registry root is not a directory: " + registry_path.string());
    }
    const runtime::core::runtime_graph::RuntimeRegistry registry(registry_path);
    for (const auto& runtime : registry.DiscoverLocalRuntimes()) {
      if (!runtime.services) {
        status.status = DiscoveryStatus::kError;
        if (!status.message.empty()) status.message += ' ';
        status.message += "Runtime '" + runtime.runtime_id +
            "' does not advertise service information; upgrade and restart it.";
        continue;
      }
      for (const auto& service : *runtime.services) {
        result.services.push_back({.source = source, .runtime_id = runtime.runtime_id,
                                   .advertisement = service});
      }
    }
    status.observed_at = std::chrono::system_clock::now();
  } catch (const std::exception& error) {
    status.status = DiscoveryStatus::kError;
    status.message = error.what();
  }
  result.sources.push_back(std::move(status));
  return result;
}

}  // namespace ibmw::discovery
