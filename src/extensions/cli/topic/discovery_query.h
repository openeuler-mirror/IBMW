// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cli/command.h"
#include "engine/discovery/discovery_provider.h"

namespace ibmw::cli::topic {

ibmw::discovery::DiscoverySnapshot DiscoverTopics(const Invocation& invocation);
bool IsVisible(
    const ibmw::discovery::DiscoveredEndpoint& endpoint, bool include_hidden_topics);
std::string_view TopicName(const ibmw::discovery::DiscoveredEndpoint& endpoint);
std::string_view TypeName(const ibmw::discovery::DiscoveredEndpoint& endpoint);
std::vector<std::string> VisibleTopicNames(
    const ibmw::discovery::DiscoverySnapshot& snapshot, bool include_hidden_topics);
std::vector<ibmw::discovery::DiscoveredEndpoint> VisibleEndpoints(
    const ibmw::discovery::DiscoverySnapshot& snapshot,
    std::string_view topic_name,
    bool include_hidden_topics);
std::set<std::string> TopicTypes(
    const std::vector<ibmw::discovery::DiscoveredEndpoint>& endpoints);

}  // namespace ibmw::cli::topic
