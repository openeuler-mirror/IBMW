// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <map>
#include <set>
#include <stdexcept>
#include "engine/discovery/discovery_provider.h"
#include "engine/discovery/service_discovery_protocol.h"

namespace ibmw::discovery {
namespace {
class Provider final : public ServiceDiscoveryProvider {
 public:
  ServiceDiscoverySnapshot Query(const ServiceDiscoveryOptions& options,
      std::chrono::steady_clock::time_point deadline) override {
    auto observer = CreateDiscoveryProvider(DiscoveryBackend::kDds);
    if (!observer) throw std::runtime_error("DDS discovery plugin is not installed.");
    observer->Start({.backend = DiscoveryBackend::kDds, .domain_id = options.domain_id});
    const auto endpoints = observer->Query(deadline);
    observer->Stop();
    if (!endpoints.Succeeded()) throw std::runtime_error("DDS endpoint discovery failed.");
    struct Pair { std::set<std::string> requests, responses; std::string name; };
    std::map<std::pair<std::string, std::string>, Pair> pairs;
    for (const auto& endpoint : endpoints.endpoints) {
      const auto& topic = endpoint.wire_topic_name;
      const bool request = endpoint.role == EndpointRole::kSubscriber;
      const auto prefix = request ? "rq/" : "rr/";
      const auto suffix = request ? "Request" : "Reply";
      std::string name, convention;
      if (topic.starts_with(prefix) && topic.ends_with(suffix)) {
        name = topic.substr(3, topic.size() - 3 - std::char_traits<char>::length(suffix));
        convention = "ros2/";
      } else if (topic.starts_with(request ? "ibmw_rpc/rq/" : "ibmw_rpc/rr/")) {
        name = topic.substr(12); convention = "ibmw/";
      } else continue;
      if (name.empty() || endpoint.wire_type_name.empty()) continue;
      auto& pair = pairs[{endpoint.participant_id, convention + name}];
      pair.name = '/' + name;
      (request ? pair.requests : pair.responses).insert(endpoint.wire_type_name);
    }
    ServiceDiscoverySnapshot result;
    const auto source = DescribeSource(options);
    for (const auto& [identity, pair] : pairs) {
      // A client has the opposite endpoint roles and cannot create a service.
      if (pair.requests.empty() || pair.responses.empty()) continue;
      if (pair.requests.size() != 1 || pair.responses.size() != 1)
        throw std::runtime_error("Ambiguous DDS service types for " + pair.name);
      result.services.push_back({.source = source, .runtime_id = identity.first,
          .advertisement = {.endpoint_id = identity.second, .service_name = pair.name,
              .request_type = *pair.requests.begin(), .response_type = *pair.responses.begin(),
              .available_drivers = {"dds"}, .hidden = service_protocol::IsHidden(pair.name)}});
    }
    result.sources.push_back({.source = source, .status = DiscoveryStatus::kReady});
    return result;
  }
};
}  // namespace
}  // namespace ibmw::discovery

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw::discovery::ServiceDiscoveryPluginV1 IbmwServiceDiscoveryEntryV1() {
  using namespace ibmw::discovery;
  return {.backend = ServiceDiscoveryBackend::kDds,
      .create = [] { return static_cast<ServiceDiscoveryProvider*>(new Provider); },
      .destroy = [](ServiceDiscoveryProvider* provider) noexcept { delete provider; }};
}
