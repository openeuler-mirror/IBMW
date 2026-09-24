// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "discovery_query.h"

#include <algorithm>
#include <iostream>

namespace ibmw::cli::topic {

using ibmw::cli::Invocation;
using ibmw::runtime::core::runtime_graph::RuntimeRegistry;
using ibmw::runtime::core::runtime_graph::TopicAdvertisement;
using ibmw::runtime::core::runtime_graph::TopicGraph;

TopicGraph DiscoverTopics(const Invocation& invocation) {
  const RuntimeRegistry registry(invocation.OptionValue(
      "--registry-path", RuntimeRegistry().RegistryPath().string()));
  TopicGraph graph;
  graph.ReplaceRuntimes(registry.DiscoverLocalRuntimes());
  return graph;
}

bool IsVisible(
    const TopicAdvertisement& advertisement, bool include_hidden_topics) {
  return include_hidden_topics || !advertisement.hidden;
}

std::vector<std::string> VisibleTopicNames(
    const TopicGraph& graph, bool include_hidden_topics) {
  std::vector<std::string> result;
  for (const auto& topic_name : graph.Topics()) {
    const auto advertisements = graph.Find(topic_name);
    const bool visible = std::any_of(
        advertisements.begin(), advertisements.end(),
        [include_hidden_topics](const auto& advertisement) {
          return IsVisible(advertisement, include_hidden_topics);
        });
    if (visible) {
      result.emplace_back(topic_name);
    }
  }
  return result;
}

std::vector<TopicAdvertisement> VisibleAdvertisements(
    const TopicGraph& graph,
    std::string_view topic_name,
    bool include_hidden_topics) {
  std::vector<TopicAdvertisement> result;
  for (const auto& advertisement : graph.Find(topic_name)) {
    if (IsVisible(advertisement, include_hidden_topics)) {
      result.emplace_back(advertisement);
    }
  }
  return result;
}

std::set<std::string> TopicTypes(
    const std::vector<TopicAdvertisement>& advertisements) {
  std::set<std::string> result;
  for (const auto& advertisement : advertisements) {
    result.emplace(advertisement.topic.msg_type);
  }
  return result;
}

}  // namespace ibmw::cli::topic
