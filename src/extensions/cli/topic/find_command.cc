// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "find_command.h"

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

int RunTopicFindCommandImpl(const Invocation& invocation) {
  const std::string requested_type = invocation.positional_args.front();
  const bool include_hidden_topics =
      invocation.HasOption("--include-hidden-topics");
  const auto graph = DiscoverTopics(invocation);
  std::vector<std::string> matching_topics;
  for (const auto& topic_name :
       VisibleTopicNames(graph, include_hidden_topics)) {
    const auto advertisements =
        VisibleAdvertisements(graph, topic_name, include_hidden_topics);
    const bool matches = std::any_of(
        advertisements.begin(), advertisements.end(),
        [&requested_type](const auto& advertisement) {
          return advertisement.topic.msg_type == requested_type;
        });
    if (matches) {
      matching_topics.emplace_back(topic_name);
    }
  }

  if (invocation.HasOption("--count-topics")) {
    std::cout << matching_topics.size() << '\n';
    return 0;
  }
  for (const auto& topic_name : matching_topics) {
    std::cout << topic_name << '\n';
  }
  return 0;
}

}  // namespace

void AddFindCommand(Command& topic) {
  const std::string registry_description =
      "Runtime registration directory (default: " +
      ibmw::runtime::core::runtime_graph::RuntimeRegistry().RegistryPath().string() + ")";

  auto& find = topic.AddSubcommand("find", "Find topics by message type");
  find.SetHandler(RunTopicFindCommandImpl);
  find.AddArgument("<type>", "Message type");
  find.AddOption(
      "--count-topics", "-c", {}, "Only display the number of topics");
  find.AddOption(
      "--include-hidden-topics", {}, {},
      "Consider hidden topics as well");
  find.AddOption(
      "--registry-path", {}, "path",
      registry_description,
      ibmw::cli::OptionValueMode::kRequired);
}

}  // namespace ibmw::cli::topic
