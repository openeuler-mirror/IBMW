// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "list_command.h"

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

int RunTopicListCommandImpl(const Invocation& invocation) {
  const bool include_hidden_topics =
      invocation.HasOption("--include-hidden-topics");
  const auto graph = DiscoverTopics(invocation);
  const auto topic_names =
      VisibleTopicNames(graph, include_hidden_topics);

  if (invocation.HasOption("--count-topics")) {
    std::cout << topic_names.size() << '\n';
    return 0;
  }

  const bool show_types = invocation.HasOption("--show-types");
  const bool verbose = invocation.HasOption("--verbose");
  for (const auto& topic_name : topic_names) {
    const auto advertisements =
        VisibleAdvertisements(graph, topic_name, include_hidden_topics);
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
        if (advertisement.published) {
          ++publisher_count;
        } else {
          ++subscriber_count;
        }
      }
      std::cout << "  publishers: " << publisher_count << '\n';
      std::cout << "  subscribers: " << subscriber_count << '\n';
    }
  }
  return 0;
}

}  // namespace

void AddListCommand(Command& topic) {
  const std::string registry_description =
      "Runtime registration directory (default: " +
      ibmw::runtime::core::runtime_graph::RuntimeRegistry().RegistryPath().string() + ")";

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
  list.AddOption(
      "--registry-path", {}, "path",
      registry_description,
      ibmw::cli::OptionValueMode::kRequired);
}

}  // namespace ibmw::cli::topic
