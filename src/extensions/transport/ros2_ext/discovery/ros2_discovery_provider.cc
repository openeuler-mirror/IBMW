// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "extension_api/ibmw_discovery_provider.h"

#include <array>
#include <memory>
#include <stdexcept>
#include <thread>

#include <rclcpp/context.hpp>
#include <rclcpp/node_interfaces/node_base.hpp>
#include <rclcpp/node_interfaces/node_graph.hpp>
#include <rclcpp/node_options.hpp>
#include <rmw/qos_string_conversions.h>
#include <rmw/rmw.h>

namespace ibmw::discovery {
namespace {

std::string GidString(const std::array<uint8_t, RMW_GID_STORAGE_SIZE>& gid) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(gid.size() * 2);
  for (const auto byte : gid) {
    result += hex[byte >> 4];
    result += hex[byte & 0x0f];
  }
  return result;
}

std::string DescribeDuration(const rmw_time_t& duration) {
  constexpr rmw_time_t infinite = RMW_DURATION_INFINITE;
  if (duration.sec == infinite.sec && duration.nsec == infinite.nsec) return "infinite";
  if (duration.sec == 0 && duration.nsec == 0) return "unspecified";
  return std::to_string(duration.sec) + " s " + std::to_string(duration.nsec) + " ns";
}

ibmw::transport::QosDescriptor DescribeQos(const rmw_qos_profile_t& qos) {
  ibmw::transport::QosDescriptor result;
  const auto add = [&](const char* name, const char* value) {
    if (value) result.policies[name] = value;
  };
  add("reliability", rmw_qos_reliability_policy_to_str(qos.reliability));
  add("durability", rmw_qos_durability_policy_to_str(qos.durability));
  add("liveliness", rmw_qos_liveliness_policy_to_str(qos.liveliness));
  add("history", rmw_qos_history_policy_to_str(qos.history));
  result.policies["deadline"] = DescribeDuration(qos.deadline);
  result.policies["lifespan"] = DescribeDuration(qos.lifespan);
  result.policies["liveliness_lease_duration"] = DescribeDuration(qos.liveliness_lease_duration);
  if (qos.history == RMW_QOS_POLICY_HISTORY_KEEP_LAST && qos.depth != 0) {
    result.policies["depth"] = std::to_string(qos.depth);
  }
  return result;
}

class Provider final : public DiscoveryProvider {
 public:
  ~Provider() override { Stop(); }

  void Start(const DiscoveryOptions& options) override {
    if (context_) throw std::logic_error("ROS 2 discovery is already started.");
    if (options.backend != DiscoveryBackend::kRos2) throw std::invalid_argument("Invalid ROS 2 discovery source.");
    source_ = DescribeSource(options);
    context_ = std::make_shared<rclcpp::Context>();
    rclcpp::InitOptions init_options;
    init_options.set_domain_id(options.domain_id);
    init_options.shutdown_on_signal = false;
    init_options.auto_initialize_logging(false);
    context_->init(0, nullptr, init_options);

    rclcpp::NodeOptions node_options;
    node_options.use_global_arguments(false).enable_rosout(false);
    // A full rclcpp::Node also creates a TimeSource parameter-event subscriber.
    // Only construct the base and graph interfaces needed for discovery.
    node_ = std::make_shared<rclcpp::node_interfaces::NodeBase>(
        "_ibmw_discovery", "/", context_, *node_options.get_rcl_node_options(), false, false);
    graph_ = std::make_unique<rclcpp::node_interfaces::NodeGraph>(node_.get());
  }

  DiscoverySnapshot Query(std::chrono::steady_clock::time_point deadline) override {
    if (!graph_) throw std::logic_error("ROS 2 discovery is not started.");
    // The RMW maintains the graph on its own threads; no executor or business
    // subscription is needed to collect a snapshot at the shared deadline.
    std::this_thread::sleep_until(deadline);
    DiscoverySnapshot result;
    const char* serialization = rmw_get_serialization_format();
    for (const auto& [topic, types] : graph_->get_topic_names_and_types()) {
      for (const auto& info : graph_->get_publishers_info_by_topic(topic)) {
        result.endpoints.push_back(DescribeEndpoint(topic, info, EndpointRole::kPublisher, serialization));
      }
      for (const auto& info : graph_->get_subscriptions_info_by_topic(topic)) {
        result.endpoints.push_back(DescribeEndpoint(topic, info, EndpointRole::kSubscriber, serialization));
      }
    }
    result.sources.push_back({.source = source_, .status = DiscoveryStatus::kReady,
                              .observed_at = std::chrono::system_clock::now()});
    return result;
  }

  void Stop() noexcept override {
    graph_.reset();
    node_.reset();
    if (context_) {
      try {
        if (context_->is_valid()) context_->shutdown("ROS 2 discovery stopped");
      } catch (...) {
        // Stop is also used while unwinding a failed initialization.
      }
      context_.reset();
    }
  }

 private:
  DiscoveredEndpoint DescribeEndpoint(const std::string& topic,
      const rclcpp::TopicEndpointInfo& info, EndpointRole role, const char* serialization) const {
    DiscoveredEndpoint result{.source = source_, .endpoint_id = GidString(info.endpoint_gid()),
                              .role = role, .node_name = info.node_name(),
                              .node_namespace = info.node_namespace(),
                              .topic_name = topic, .type_name = info.topic_type()};
    // RMW GIDs are opaque: do not infer a participant GUID or DDS wire names.
    if (serialization) result.serialization_type = serialization;
    result.qos = DescribeQos(info.qos_profile().get_rmw_qos_profile());
    result.available_drivers = {"ros2"};
    result.hidden = topic.starts_with('_') || topic.find("/_") != std::string::npos;
    return result;
  }

  DiscoverySource source_;
  rclcpp::Context::SharedPtr context_;
  rclcpp::node_interfaces::NodeBase::SharedPtr node_;
  std::unique_ptr<rclcpp::node_interfaces::NodeGraph> graph_;
};
}  // namespace
}  // namespace ibmw::discovery

extern "C" IBMW_CORE_EXTENSION_EXPORT
ibmw::discovery::DiscoveryPluginV1 IbmwDiscoveryEntryV1() {
  using namespace ibmw::discovery;
  return {.backend = DiscoveryBackend::kRos2,
          .create = [] { return static_cast<DiscoveryProvider*>(new Provider); },
          .destroy = [](DiscoveryProvider* provider) noexcept { delete provider; }};
}
