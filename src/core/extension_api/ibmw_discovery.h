// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/transport/topic_descriptor.h"

namespace ibmw::discovery {

enum class DiscoveryBackend { kRegistry, kDds, kRos2, kZenoh };

inline std::string_view BackendName(DiscoveryBackend backend) noexcept {
  switch (backend) {
    case DiscoveryBackend::kRegistry: return "registry";
    case DiscoveryBackend::kDds: return "dds";
    case DiscoveryBackend::kRos2: return "ros2";
    case DiscoveryBackend::kZenoh: return "zenoh";
  }
  return "unknown";
}

struct DiscoverySource {
  DiscoveryBackend backend = DiscoveryBackend::kRegistry;
  // Decimal domain ID for DDS/ROS 2; business namespace for Zenoh.
  std::string domain;
  // Identifies the observer configuration, e.g. the local registry directory.
  std::string instance_id;

  bool operator==(const DiscoverySource&) const = default;
};

enum class EndpointRole { kPublisher, kSubscriber };

// Descriptive metadata only. Paths to libraries and runtime control handles
// belong to the local control plane, not to discovered network endpoints.
struct DiscoveredEndpoint {
  DiscoverySource source;
  std::string participant_id;
  std::string endpoint_id;
  EndpointRole role = EndpointRole::kPublisher;
  // ROS graph node identity; it is not a DDS participant identity.
  std::string node_name;
  std::string node_namespace;
  // Unknown logical or wire names remain empty; adapters must not guess them.
  std::string topic_name;
  std::string type_name;
  std::string wire_topic_name;
  std::string wire_type_name;
  std::string serialization_type;
  ibmw::transport::QosDescriptor qos;
  std::vector<std::string> available_drivers;
  bool hidden = false;
  std::string module_name;
  std::map<std::string, ibmw::transport::QosDescriptor> driver_qos;
};

enum class DiscoveryStatus { kReady, kDiscovering, kUnavailable, kError };

struct DiscoverySourceStatus {
  DiscoverySource source;
  DiscoveryStatus status = DiscoveryStatus::kDiscovering;
  std::string message;
  std::optional<std::chrono::system_clock::time_point> observed_at;
};

struct DiscoverySnapshot {
  std::vector<DiscoveredEndpoint> endpoints;
  std::vector<DiscoverySourceStatus> sources;

  // Ready means a snapshot was collected, not that the entire network is known.
  bool Succeeded() const noexcept {
    if (sources.empty()) return false;
    for (const auto& source : sources) {
      if (source.status != DiscoveryStatus::kReady) return false;
    }
    return true;
  }
};

}  // namespace ibmw::discovery
