// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "list_command.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "discovery_options.h"
#include "discovery_query.h"

namespace ibmw::cli::topic {
namespace {

using ibmw::cli::Invocation;
using ibmw::discovery::DiscoveredEndpoint;
using ibmw::discovery::DiscoverySnapshot;
using ibmw::discovery::DiscoveryStatus;
using ibmw::discovery::EndpointRole;

int RunTopicListCommandImpl(const Invocation& invocation) {
  const bool include_hidden_topics =
      invocation.HasOption("--include-hidden-topics");
  const auto graph = DiscoverTopics(invocation);
  const auto topic_names =
      VisibleTopicNames(graph, include_hidden_topics);

  if (invocation.HasOption("--count-topics")) {
    std::cout << topic_names.size() << '\n';
    return graph.Succeeded() ? 0 : 1;
  }

  const bool show_types = invocation.HasOption("--show-types");
  const bool verbose = invocation.HasOption("--verbose");
  for (const auto& topic_name : topic_names) {
    const auto advertisements =
        VisibleEndpoints(graph, topic_name, include_hidden_topics);
    std::cout << topic_name;
    if (show_types || verbose) {
      std::cout << " [";
      bool first = true;
      for (const auto& type : TopicTypes(advertisements)) {
        if (!first) {
          std::cout << ", ";
        }
        first = false;
        std::cout << type;
      }
      std::cout << ']';
    }
    std::cout << '\n';

    if (verbose) {
      size_t publisher_count = 0;
      size_t subscriber_count = 0;
      for (const auto& advertisement : advertisements) {
        if (advertisement.role == EndpointRole::kPublisher) {
          ++publisher_count;
        } else {
          ++subscriber_count;
        }
      }
      std::cout << "  publishers: " << publisher_count << '\n';
      std::cout << "  subscribers: " << subscriber_count << '\n';
    }
  }
  return graph.Succeeded() ? 0 : 1;
}

}  // namespace

void AddListCommand(Command& topic) {
  auto& list = topic.AddSubcommand("list", "List discovered topics");
  list.SetHandler(RunTopicListCommandImpl);
  list.AddOption(
      "--show-types", "-t", {}, "Additionally show the topic type");
  list.AddOption(
      "--count-topics", "-c", {}, "Only display the number of topics");
  list.AddOption(
      "--verbose", "-v", {}, "Show publisher and subscriber details");
  list.AddOption(
      "--include-hidden-topics", {}, {},
      "Consider hidden topics as well");
  AddDiscoveryOptions(list);
}

}  // namespace ibmw::cli::topic
