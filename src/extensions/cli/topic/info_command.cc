// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "info_command.h"

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

void PrintQos(const ibmw::transport::QosDescriptor& qos, std::string_view indent) {
  if (qos.Empty()) {
    std::cout << indent << "qos: unavailable (not reported by this source)\n";
    return;
  }
  std::cout << indent << "qos:\n";
  for (const auto& [name, value] : qos.policies) {
    std::cout << indent << "  " << name << ": "
              << (value.empty() ? "unknown (not reported by this source)" : value) << '\n';
  }
  for (const std::string name : {"history", "depth", "reliability", "durability",
                                  "lifespan", "deadline", "liveliness",
                                  "liveliness_lease_duration"}) {
    if (!qos.policies.contains(name)) {
      std::cout << indent << "  " << name << ": unknown (not reported by this source)\n";
    }
  }
}

void PrintAdvertisement(
    const TopicAdvertisement& advertisement, std::string_view role) {
  std::cout << "  " << role << ":\n";
  std::cout << "    runtime: " << advertisement.runtime_id << '\n';
  if (!advertisement.module_name.empty()) {
    std::cout << "    module: " << advertisement.module_name << '\n';
  }
  std::cout << "    endpoint: " << advertisement.publisher_id << '\n';
  std::cout << "    type: " << advertisement.topic.msg_type << '\n';
  std::cout << "    serialization: "
            << advertisement.topic.serialization_type << '\n';
  if (!advertisement.topic.available_drivers.empty()) {
    std::cout << "    drivers: ";
    for (size_t index = 0;
         index < advertisement.topic.available_drivers.size(); ++index) {
      if (index != 0) {
        std::cout << ", ";
      }
      std::cout << advertisement.topic.available_drivers[index];
    }
    std::cout << '\n';
  }
  if (!advertisement.driver_qos.empty()) {
    for (const auto& [driver, qos] : advertisement.driver_qos) {
      std::cout << "    driver: " << driver << '\n';
      PrintQos(qos, "      ");
    }
  } else {
    PrintQos({}, "    ");
  }
}

int RunTopicInfoCommandImpl(const Invocation& invocation) {
  const std::string topic_name = invocation.positional_args.front();
  const auto graph = DiscoverTopics(invocation);
  // Like ROS 2, an explicit info query also resolves hidden topics.
  const auto advertisements = VisibleAdvertisements(graph, topic_name, true);
  if (advertisements.empty()) {
    std::cerr << "ibmw: error: Unknown topic '" << topic_name << "'.\n";
    return 1;
  }

  const auto types = TopicTypes(advertisements);
  std::cout << "Type: ";
  bool first = true;
  for (const auto& type : types) {
    if (!first) {
      std::cout << ", ";
    }
    first = false;
    std::cout << type;
  }
  std::cout << '\n';

  size_t publisher_count = 0;
  size_t subscriber_count = 0;
  for (const auto& advertisement : advertisements) {
    if (advertisement.published) {
      ++publisher_count;
    } else {
      ++subscriber_count;
    }
  }
  std::cout << "Publisher count: " << publisher_count << '\n';
  std::cout << "Subscription count: " << subscriber_count << '\n';

  if (invocation.HasOption("--verbose")) {
    std::cout << "\nPublishers:\n";
    for (const auto& advertisement : advertisements) {
      if (advertisement.published) {
        PrintAdvertisement(advertisement, "publisher");
      }
    }
    std::cout << "\nSubscribers:\n";
    for (const auto& advertisement : advertisements) {
      if (!advertisement.published) {
        PrintAdvertisement(advertisement, "subscriber");
      }
    }
  }
  return 0;
}

}  // namespace

void AddInfoCommand(Command& topic) {
  const std::string registry_description =
      "Runtime registration directory (default: " +
      ibmw::runtime::core::runtime_graph::RuntimeRegistry().RegistryPath().string() + ")";

  auto& info = topic.AddSubcommand("info", "Show information about a topic");
  info.SetHandler(RunTopicInfoCommandImpl);
  info.AddArgument("<topic>", "Topic name");
  info.AddOption(
      "--verbose", "-v", {}, "Show endpoint and QoS details");
  info.AddOption(
      "--include-hidden-topics", {}, {},
      "Consider hidden topics as well");
  info.AddOption(
      "--registry-path", {}, "path",
      registry_description,
      ibmw::cli::OptionValueMode::kRequired);
}

}  // namespace ibmw::cli::topic
