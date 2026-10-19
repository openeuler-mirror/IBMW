// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "discovery_query.h"

#include <algorithm>
#include <iostream>

#include "discovery_options.h"

namespace ibmw::cli::topic {

using ibmw::cli::Invocation;
using ibmw::discovery::DiscoveredEndpoint;
using ibmw::discovery::DiscoverySnapshot;
using ibmw::discovery::DiscoveryStatus;
using ibmw::discovery::EndpointRole;

DiscoverySnapshot DiscoverTopics(const Invocation& invocation) {
  auto snapshot = ibmw::discovery::Discover(
      ibmw::cli::topic::ParseDiscoveryOptions(invocation));
  for (const auto& status : snapshot.sources) {
    if (status.status == DiscoveryStatus::kReady) continue;
    std::cerr << "ibmw topic: error: " << ibmw::discovery::BackendName(status.source.backend);
    if (!status.source.domain.empty()) {
      std::cerr << " (domain " << status.source.domain << ')';
    }
    std::cerr << ": " << status.message << '\n';
  }
  return snapshot;
}

bool IsVisible(
    const DiscoveredEndpoint& endpoint, bool include_hidden_topics) {
  return include_hidden_topics || !endpoint.hidden;
}

std::string_view TopicName(const DiscoveredEndpoint& endpoint) {
  return endpoint.topic_name.empty() ? endpoint.wire_topic_name : endpoint.topic_name;
}

std::string_view TypeName(const DiscoveredEndpoint& endpoint) {
  return endpoint.type_name.empty() ? endpoint.wire_type_name : endpoint.type_name;
}

std::vector<std::string> VisibleTopicNames(
    const DiscoverySnapshot& snapshot, bool include_hidden_topics) {
  std::set<std::string> result;
  for (const auto& endpoint : snapshot.endpoints) {
    if (IsVisible(endpoint, include_hidden_topics)) {
      result.emplace(TopicName(endpoint));
    }
  }
  return {result.begin(), result.end()};
}

std::vector<DiscoveredEndpoint> VisibleEndpoints(
    const DiscoverySnapshot& snapshot,
    std::string_view topic_name,
    bool include_hidden_topics) {
  std::vector<DiscoveredEndpoint> result;
  for (const auto& endpoint : snapshot.endpoints) {
    if (TopicName(endpoint) == topic_name && IsVisible(endpoint, include_hidden_topics)) {
      result.emplace_back(endpoint);
    }
  }
  return result;
}

std::set<std::string> TopicTypes(
    const std::vector<DiscoveredEndpoint>& endpoints) {
  std::set<std::string> result;
  for (const auto& endpoint : endpoints) {
    if (!TypeName(endpoint).empty()) result.emplace(TypeName(endpoint));
  }
  return result;
}

}  // namespace ibmw::cli::topic
