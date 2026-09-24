// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "type_command.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "discovery_query.h"

namespace ibmw::cli::topic {
namespace {

using ibmw::cli::Invocation;
using ibmw::runtime::core::runtime_graph::RuntimeRegistry;
using ibmw::runtime::core::runtime_graph::TopicAdvertisement;
using ibmw::runtime::core::runtime_graph::TopicGraph;

int RunTopicTypeCommandImpl(const Invocation& invocation) {
  const std::string topic_name = invocation.positional_args.front();
  const auto graph = DiscoverTopics(invocation);
  const auto advertisements = VisibleAdvertisements(
      graph, topic_name,
      invocation.HasOption("--include-hidden-topics"));
  if (advertisements.empty()) {
    std::cerr << "ibmw: error: Unknown topic '" << topic_name << "'.\n";
    return 1;
  }

  for (const auto& type : TopicTypes(advertisements)) {
    std::cout << type << '\n';
  }
  return 0;
}

}  // namespace

void AddTypeCommand(Command& topic) {
  const std::string registry_description =
      "Runtime registration directory (default: " +
      ibmw::runtime::core::runtime_graph::RuntimeRegistry().RegistryPath().string() + ")";

  auto& type = topic.AddSubcommand("type", "Print a topic's type");
  type.SetHandler(RunTopicTypeCommandImpl);
  type.AddArgument("<topic>", "Topic name");
  type.AddOption(
      "--include-hidden-topics", {}, {},
      "Consider hidden topics as well");
  type.AddOption(
      "--registry-path", {}, "path",
      registry_description,
      ibmw::cli::OptionValueMode::kRequired);
}

}  // namespace ibmw::cli::topic
