// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "find_command.h"

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

int RunTopicFindCommandImpl(const Invocation& invocation) {
  const std::string requested_type = invocation.positional_args.front();
  const bool include_hidden_topics =
      invocation.HasOption("--include-hidden-topics");
  const auto graph = DiscoverTopics(invocation);
  std::vector<std::string> matching_topics;
  for (const auto& topic_name :
       VisibleTopicNames(graph, include_hidden_topics)) {
    const auto advertisements =
        VisibleEndpoints(graph, topic_name, include_hidden_topics);
    const bool matches = std::any_of(
        advertisements.begin(), advertisements.end(),
        [&requested_type](const auto& advertisement) {
          return TypeName(advertisement) == requested_type;
        });
    if (matches) {
      matching_topics.emplace_back(topic_name);
    }
  }

  if (invocation.HasOption("--count-topics")) {
    std::cout << matching_topics.size() << '\n';
    return graph.Succeeded() ? 0 : 1;
  }
  for (const auto& topic_name : matching_topics) {
    std::cout << topic_name << '\n';
  }
  return graph.Succeeded() ? 0 : 1;
}

}  // namespace

void AddFindCommand(Command& topic) {
  auto& find = topic.AddSubcommand("find", "Find topics by message type");
  find.SetHandler(RunTopicFindCommandImpl);
  find.AddArgument("<type>", "Message type");
  find.AddOption(
      "--count-topics", "-c", {}, "Only display the number of topics");
  find.AddOption(
      "--include-hidden-topics", {}, {},
      "Consider hidden topics as well");
  AddDiscoveryOptions(find);
}

}  // namespace ibmw::cli::topic
