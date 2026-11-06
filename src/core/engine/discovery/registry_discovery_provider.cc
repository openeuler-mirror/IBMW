// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/discovery/registry_discovery_provider.h"

#include <stdexcept>

#include "engine/runtime_graph/topic_graph.h"

namespace ibmw::discovery {

void RegistryDiscoveryProvider::Start(const DiscoveryOptions& options) {
  if (registry_) throw std::logic_error("Registry discovery is already started.");
  if (options.backend != DiscoveryBackend::kRegistry || options.registry_path.empty()) {
    throw std::invalid_argument("Registry discovery requires a registry source and registry path.");
  }
  source_ = DescribeSource(options);
  registry_.emplace(options.registry_path);
}

DiscoverySnapshot RegistryDiscoveryProvider::Query(
    std::chrono::steady_clock::time_point deadline) {
  static_cast<void>(deadline);
  return QueryImpl(true);
}

DiscoverySnapshot RegistryDiscoveryProvider::QueryReadOnly() {
  return QueryImpl(false);
}

DiscoverySnapshot RegistryDiscoveryProvider::QueryImpl(bool cleanup_stale) {
  if (!registry_) throw std::logic_error("Registry discovery has not been started.");
  ibmw::runtime::core::runtime_graph::TopicGraph graph;
  runtimes_ = cleanup_stale ? registry_->DiscoverLocalRuntimes()
                            : registry_->InspectLocalRuntimes();
  graph.ReplaceRuntimes(runtimes_);

  DiscoverySnapshot result;
  for (const auto& name : graph.Topics()) {
    for (const auto& advertisement : graph.Find(name)) {
      result.endpoints.push_back(DiscoveredEndpoint{
          .source = source_,
          .participant_id = advertisement.runtime_id,
          .endpoint_id = advertisement.publisher_id,
          .role = advertisement.published ? EndpointRole::kPublisher
                                          : EndpointRole::kSubscriber,
          .topic_name = advertisement.topic.topic,
          .type_name = advertisement.topic.msg_type,
          .serialization_type = advertisement.topic.serialization_type,
          .available_drivers = advertisement.topic.available_drivers,
          .hidden = advertisement.hidden,
          .module_name = advertisement.module_name,
          .driver_qos = advertisement.driver_qos});
    }
  }
  result.sources.push_back(DiscoverySourceStatus{
      .source = source_, .status = DiscoveryStatus::kReady,
      .observed_at = std::chrono::system_clock::now()});
  return result;
}

void RegistryDiscoveryProvider::Stop() noexcept {
  runtimes_.clear();
  registry_.reset();
}

}  // namespace ibmw::discovery
