// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <memory>
#include <thread>
#include <rclcpp/context.hpp>
#include <rclcpp/node_interfaces/node_base.hpp>
#include <rclcpp/node_interfaces/node_graph.hpp>
#include <rclcpp/node_options.hpp>
#include "engine/discovery/service_discovery_protocol.h"

namespace ibmw::discovery {
namespace {
class Provider final : public ServiceDiscoveryProvider {
 public:
  ServiceDiscoverySnapshot Query(const ServiceDiscoveryOptions& options,
      std::chrono::steady_clock::time_point deadline) override {
    auto context = std::make_shared<rclcpp::Context>();
    rclcpp::InitOptions init;
    init.set_domain_id(options.domain_id);
    init.shutdown_on_signal = false;
    init.auto_initialize_logging(false);
    context->init(0, nullptr, init);
    rclcpp::NodeOptions node_options;
    node_options.use_global_arguments(false).enable_rosout(false);
    auto node = std::make_shared<rclcpp::node_interfaces::NodeBase>(
        ("_ibmw_service_discovery_" + service_protocol::NewInstanceId()).c_str(), "/", context,
        *node_options.get_rcl_node_options(), false, false);
    auto graph = std::make_unique<rclcpp::node_interfaces::NodeGraph>(node.get());
    std::this_thread::sleep_until(deadline);
    ServiceDiscoverySnapshot result;
    const auto source = DescribeSource(options);
    // Query server registrations by node, never client-only graph entries.
    for (const auto& [node_name, node_namespace] : graph->get_node_names_and_namespaces()) {
      for (const auto& [name, types] : graph->get_service_names_and_types_by_node(node_name, node_namespace)) {
        for (const auto& type : types) {
          result.services.push_back({.source = source, .runtime_id = node_namespace + '/' + node_name,
              .advertisement = {.endpoint_id = name, .service_name = name,
                  .available_drivers = {"ros2"}, .hidden = service_protocol::IsHidden(name)},
              .type_name = type});
        }
      }
    }
    graph.reset(); node.reset(); context->shutdown("Service discovery complete");
    result.sources.push_back({.source = source, .status = DiscoveryStatus::kReady});
    return result;
  }
};
}  // namespace
}  // namespace ibmw::discovery

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw::discovery::ServiceDiscoveryPluginV1 IbmwServiceDiscoveryEntryV1() {
  using namespace ibmw::discovery;
  return {.backend = ServiceDiscoveryBackend::kRos2,
      .create = [] { return static_cast<ServiceDiscoveryProvider*>(new Provider); },
      .destroy = [](ServiceDiscoveryProvider* provider) noexcept { delete provider; }};
}
