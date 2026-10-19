// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include "extension_api/ibmw_discovery.h"
#include "extension_api/ibmw_extension_export.h"
#include "extension_api/ibmw_runtime_directory.h"

namespace ibmw::discovery {

struct DiscoveryOptions {
  DiscoveryBackend backend = DiscoveryBackend::kRegistry;
  std::filesystem::path registry_path = ibmw::extension::DefaultRuntimeRegistryPath();
  uint32_t domain_id = 0;
  std::string zenoh_domain;
  std::filesystem::path zenoh_config;
};

inline DiscoverySource DescribeSource(const DiscoveryOptions& options) {
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

class DiscoveryProvider {
 public:
  virtual ~DiscoveryProvider() = default;
  virtual void Start(const DiscoveryOptions&) = 0;
  virtual DiscoverySnapshot Query(std::chrono::steady_clock::time_point deadline) = 0;
  // Idempotent, including after failed Start; drains callbacks before return.
  virtual void Stop() noexcept = 0;
};

// Host and plugin must use matching headers, compiler and C++ standard-library
// ABI. Snapshots own their data; provider destruction runs in its owning library.
struct DiscoveryPluginV1 {
  uint32_t version = 1;
  uint32_t struct_size = sizeof(DiscoveryPluginV1);
  DiscoveryBackend backend = DiscoveryBackend::kRegistry;
  DiscoveryProvider* (*create)() = nullptr;
  void (*destroy)(DiscoveryProvider*) noexcept = nullptr;
};

}  // namespace ibmw::discovery

extern "C" IBMW_CORE_EXTENSION_EXPORT
ibmw::discovery::DiscoveryPluginV1 IbmwDiscoveryEntryV1();
