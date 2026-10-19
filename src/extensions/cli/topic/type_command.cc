// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "type_command.h"

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

int RunTopicTypeCommandImpl(const Invocation& invocation) {
  const std::string topic_name = invocation.positional_args.front();
  const auto graph = DiscoverTopics(invocation);
  const auto advertisements = VisibleEndpoints(
      graph, topic_name,
      invocation.HasOption("--include-hidden-topics"));
  if (advertisements.empty()) {
    if (graph.Succeeded()) {
      std::cerr << "ibmw: error: Unknown topic '" << topic_name << "'.\n";
    }
    return 1;
  }

  for (const auto& type : TopicTypes(advertisements)) {
    std::cout << type << '\n';
  }
  return graph.Succeeded() ? 0 : 1;
}

}  // namespace

void AddTypeCommand(Command& topic) {
  auto& type = topic.AddSubcommand("type", "Print a topic's type");
  type.SetHandler(RunTopicTypeCommandImpl);
  type.AddArgument("<topic>", "Topic name");
  type.AddOption(
      "--include-hidden-topics", {}, {},
      "Consider hidden topics as well");
  AddDiscoveryOptions(type);
}

}  // namespace ibmw::cli::topic
