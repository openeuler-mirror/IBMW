// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/discovery/discovery_provider.h"

#include <exception>
#include <iterator>
#include <stdexcept>

#include "engine/discovery/registry_discovery_provider.h"

namespace ibmw::discovery {

DiscoverySource DescribeSource(const DiscoveryOptions& options) {
  DiscoverySource source{.backend = options.backend};
  switch (options.backend) {
    case DiscoveryBackend::kRegistry:
      source.instance_id = options.registry_path.lexically_normal().string();
      break;
    case DiscoveryBackend::kDds:
    case DiscoveryBackend::kRos2:
      source.domain = std::to_string(options.domain_id);
      break;
    case DiscoveryBackend::kZenoh:
      source.domain = options.zenoh_domain;
      source.instance_id = options.zenoh_config.lexically_normal().string();
      break;
  }
  return source;
}

std::unique_ptr<DiscoveryProvider> CreateDiscoveryProvider(
    DiscoveryBackend backend) {
  if (backend == DiscoveryBackend::kRegistry) {
    return std::make_unique<RegistryDiscoveryProvider>();
  }
  return nullptr;
}

DiscoverySnapshot Discover(
    const std::vector<DiscoveryOptions>& sources,
    std::chrono::milliseconds timeout) {
  if (sources.empty() || timeout < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("Discovery requires sources and a nonnegative timeout.");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  DiscoverySnapshot result;
  for (const auto& options : sources) {
    const auto source = DescribeSource(options);
    auto provider = CreateDiscoveryProvider(options.backend);
    if (!provider) {
      result.sources.push_back(DiscoverySourceStatus{
          .source = source, .status = DiscoveryStatus::kUnavailable,
          .message = "Discovery backend is not implemented in this build."});
      continue;
    }
    try {
      provider->Start(options);
      auto snapshot = provider->Query(deadline);
      result.endpoints.insert(result.endpoints.end(),
                              std::make_move_iterator(snapshot.endpoints.begin()),
                              std::make_move_iterator(snapshot.endpoints.end()));
      result.sources.insert(result.sources.end(),
                            std::make_move_iterator(snapshot.sources.begin()),
                            std::make_move_iterator(snapshot.sources.end()));
    } catch (const std::exception& error) {
      result.sources.push_back(DiscoverySourceStatus{
          .source = source, .status = DiscoveryStatus::kError,
          .message = error.what()});
    } catch (...) {
      result.sources.push_back(DiscoverySourceStatus{
          .source = source, .status = DiscoveryStatus::kError,
          .message = "Unknown discovery failure."});
    }
    provider->Stop();
  }
  return result;
}

}  // namespace ibmw::discovery
