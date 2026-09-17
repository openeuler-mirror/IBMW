// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "info_command.h"

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

void PrintEndpoint(
    const DiscoveredEndpoint& endpoint, std::string_view role) {
  std::cout << "  " << role << ":\n";
  std::cout << "    source: " << ibmw::discovery::BackendName(endpoint.source.backend) << '\n';
  if (!endpoint.source.domain.empty()) {
    std::cout << "    domain: " << endpoint.source.domain << '\n';
  }
  if (!endpoint.participant_id.empty()) {
    const auto backend = endpoint.source.backend;
    const auto label = backend == ibmw::discovery::DiscoveryBackend::kRegistry ? "runtime"
                     : backend == ibmw::discovery::DiscoveryBackend::kDds ? "participant"
                     : "instance";
    std::cout << "    " << label << ": " << endpoint.participant_id << '\n';
  }
  if (!endpoint.module_name.empty()) {
    std::cout << "    module: " << endpoint.module_name << '\n';
  }
  std::cout << "    endpoint: " << endpoint.endpoint_id << '\n';
  std::cout << "    type: " << TypeName(endpoint) << '\n';
  if (!endpoint.node_name.empty()) {
    std::cout << "    node: " << endpoint.node_name << '\n';
    std::cout << "    namespace: " << endpoint.node_namespace << '\n';
  }
  std::cout << "    serialization: "
            << endpoint.serialization_type << '\n';
  if (!endpoint.available_drivers.empty()) {
    std::cout << "    drivers: ";
    for (size_t index = 0;
         index < endpoint.available_drivers.size(); ++index) {
      if (index != 0) {
        std::cout << ", ";
      }
      std::cout << endpoint.available_drivers[index];
    }
    std::cout << '\n';
  }
  if (!endpoint.driver_qos.empty()) {
    for (const auto& [driver, qos] : endpoint.driver_qos) {
      std::cout << "    driver: " << driver << '\n';
      PrintQos(qos, "      ");
    }
  } else {
    PrintQos(endpoint.qos, "    ");
  }
}

int RunTopicInfoCommandImpl(const Invocation& invocation) {
  const std::string topic_name = invocation.positional_args.front();
  const auto graph = DiscoverTopics(invocation);
  // Like ROS 2, an explicit info query also resolves hidden topics.
  const auto advertisements = VisibleEndpoints(graph, topic_name, true);
  if (advertisements.empty()) {
    if (graph.Succeeded()) {
      std::cerr << "ibmw: error: Unknown topic '" << topic_name << "'.\n";
    }
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
    if (advertisement.role == EndpointRole::kPublisher) {
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
      if (advertisement.role == EndpointRole::kPublisher) {
        PrintEndpoint(advertisement, "publisher");
      }
    }
    std::cout << "\nSubscribers:\n";
    for (const auto& advertisement : advertisements) {
      if (advertisement.role == EndpointRole::kSubscriber) {
        PrintEndpoint(advertisement, "subscriber");
      }
    }
  }
  return graph.Succeeded() ? 0 : 1;
}

}  // namespace

void AddInfoCommand(Command& topic) {
  auto& info = topic.AddSubcommand("info", "Show information about a topic");
  info.SetHandler(RunTopicInfoCommandImpl);
  info.AddArgument("<topic>", "Topic name");
  info.AddOption(
      "--verbose", "-v", {}, "Show endpoint and QoS details");
  info.AddOption(
      "--include-hidden-topics", {}, {},
      "Consider hidden topics as well");
  AddDiscoveryOptions(info);
}

}  // namespace ibmw::cli::topic
