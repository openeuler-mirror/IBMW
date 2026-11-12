// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "extension_api/ibmw_discovery.h"
#include "extension_api/ibmw_extension_export.h"
#include "extension_api/ibmw_runtime_directory.h"
#include "extension_api/ibmw_runtime_graph.h"

namespace ibmw::discovery {

// Independent of the topic discovery ABI: RPC sources have different options
// and can expose a native service type as well as request/response types.
enum class ServiceDiscoveryBackend { kRegistry, kDds, kRos2, kZenoh, kMqtt, kHttp, kGrpc };

inline std::string_view BackendName(ServiceDiscoveryBackend backend) noexcept {
  switch (backend) {
    case ServiceDiscoveryBackend::kRegistry: return "registry";
    case ServiceDiscoveryBackend::kDds: return "dds";
    case ServiceDiscoveryBackend::kRos2: return "ros2";
    case ServiceDiscoveryBackend::kZenoh: return "zenoh";
    case ServiceDiscoveryBackend::kMqtt: return "mqtt";
    case ServiceDiscoveryBackend::kHttp: return "http";
    case ServiceDiscoveryBackend::kGrpc: return "grpc";
  }
  return "unknown";
}

// Credentials stay in query options, never in source identity or wire payloads.
struct ServiceDiscoveryTlsOptions {
  std::string truststore;
  std::string client_cert;
  std::string client_key;
  bool Empty() const noexcept {
    return truststore.empty() && client_cert.empty() && client_key.empty();
  }
};

struct ServiceDiscoveryOptions {
  ServiceDiscoveryBackend backend = ServiceDiscoveryBackend::kRegistry;
  std::filesystem::path registry_path = ibmw::extension::DefaultRuntimeRegistryPath();
  uint32_t domain_id = 0;
  std::string zenoh_domain;
  std::filesystem::path zenoh_config;
  // HTTP/gRPC server URL or MQTT broker URI. Never a CLI listener address.
  std::string endpoint;
  ServiceDiscoveryTlsOptions mqtt_tls;
};

struct ServiceDiscoverySource {
  ServiceDiscoveryBackend backend = ServiceDiscoveryBackend::kRegistry;
  std::string domain;
  std::string instance_id;
};

inline ServiceDiscoverySource DescribeSource(const ServiceDiscoveryOptions& options) {
  ServiceDiscoverySource result{.backend = options.backend};
  switch (options.backend) {
    case ServiceDiscoveryBackend::kRegistry:
      result.instance_id = options.registry_path.lexically_normal().string(); break;
    case ServiceDiscoveryBackend::kDds:
    case ServiceDiscoveryBackend::kRos2:
      result.domain = std::to_string(options.domain_id); break;
    case ServiceDiscoveryBackend::kZenoh:
      result.domain = options.zenoh_domain;
      result.instance_id = options.zenoh_config.string(); break;
    default: result.instance_id = options.endpoint; break;
  }
  return result;
}

struct ServiceDiscoverySourceStatus {
  ServiceDiscoverySource source;
  DiscoveryStatus status = DiscoveryStatus::kDiscovering;
  std::string message;
};

struct DiscoveredService {
  ServiceDiscoverySource source;
  std::string runtime_id;
  ibmw::extension::ServiceAdvertisement advertisement;
  // Set only when actually supplied by a native graph (e.g. pkg/srv/Type).
  std::string type_name;
};

struct ServiceDiscoverySnapshot {
  std::vector<DiscoveredService> services;
  std::vector<ServiceDiscoverySourceStatus> sources;
  bool Succeeded() const noexcept {
    if (sources.empty()) return false;
    for (const auto& source : sources)
      if (source.status != DiscoveryStatus::kReady) return false;
    return true;
  }
};

class ServiceDiscoveryProvider {
 public:
  virtual ~ServiceDiscoveryProvider() = default;
  virtual ServiceDiscoverySnapshot Query(const ServiceDiscoveryOptions&,
      std::chrono::steady_clock::time_point deadline) = 0;
};

struct ServiceDiscoveryPluginV1 {
  uint32_t version = 1;
  uint32_t struct_size = sizeof(ServiceDiscoveryPluginV1);
  ServiceDiscoveryBackend backend = ServiceDiscoveryBackend::kRegistry;
  ServiceDiscoveryProvider* (*create)() = nullptr;
  void (*destroy)(ServiceDiscoveryProvider*) noexcept = nullptr;
};

}  // namespace ibmw::discovery

extern "C" IBMW_CORE_EXTENSION_EXPORT
ibmw::discovery::ServiceDiscoveryPluginV1 IbmwServiceDiscoveryEntryV1();
