// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cli/command.h"
#include "engine/runtime_graph/runtime_registry.h"
#include "engine/runtime_graph/topic_graph.h"

namespace ibmw::cli::topic {

ibmw::runtime::core::runtime_graph::TopicGraph DiscoverTopics(const Invocation& invocation);
bool IsVisible(
    const ibmw::runtime::core::runtime_graph::TopicAdvertisement& advertisement, bool include_hidden_topics);
std::vector<std::string> VisibleTopicNames(
    const ibmw::runtime::core::runtime_graph::TopicGraph& graph, bool include_hidden_topics);
std::vector<ibmw::runtime::core::runtime_graph::TopicAdvertisement> VisibleAdvertisements(
    const ibmw::runtime::core::runtime_graph::TopicGraph& graph,
    std::string_view topic_name,
    bool include_hidden_topics);
std::set<std::string> TopicTypes(
    const std::vector<ibmw::runtime::core::runtime_graph::TopicAdvertisement>& advertisements);

}  // namespace ibmw::cli::topic
