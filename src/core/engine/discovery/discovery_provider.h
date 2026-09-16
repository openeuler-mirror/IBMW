// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "extension_api/ibmw_discovery.h"
#include "extension_api/ibmw_runtime_directory.h"

namespace ibmw::discovery {

struct DiscoveryOptions {
  DiscoveryBackend backend = DiscoveryBackend::kRegistry;
  std::filesystem::path registry_path = ibmw::extension::DefaultRuntimeRegistryPath();
  uint32_t domain_id = 0;
  std::string zenoh_domain;
  std::filesystem::path zenoh_config;
};

// In-process interface. A future plugin entry point must define its own ABI.
class DiscoveryProvider {
 public:
  virtual ~DiscoveryProvider() = default;
  virtual void Start(const DiscoveryOptions& options) = 0;
  virtual DiscoverySnapshot Query(
      std::chrono::steady_clock::time_point deadline) = 0;
  // Idempotent, including after a failed Start; drains callbacks before return.
  virtual void Stop() noexcept = 0;
};

DiscoverySource DescribeSource(const DiscoveryOptions& options);
// Returns nullptr when this build has no discovery implementation for backend.
std::unique_ptr<DiscoveryProvider> CreateDiscoveryProvider(
    DiscoveryBackend backend);
DiscoverySnapshot Discover(
    const std::vector<DiscoveryOptions>& sources,
    std::chrono::milliseconds timeout = std::chrono::seconds(2));

}  // namespace ibmw::discovery
