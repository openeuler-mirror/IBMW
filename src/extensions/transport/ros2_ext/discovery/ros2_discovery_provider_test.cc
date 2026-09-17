// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "extension_api/ibmw_discovery_provider.h"

#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <unistd.h>

#include <gtest/gtest.h>
#include <rcl_interfaces/msg/parameter_event.hpp>
#include <rclcpp/rclcpp.hpp>

namespace ibmw::discovery {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Message = rcl_interfaces::msg::ParameterEvent;

auto MakeProvider() {
  const auto api = IbmwDiscoveryEntryV1();
  return std::unique_ptr<DiscoveryProvider, decltype(api.destroy)>(api.create(), api.destroy);
}

class Ros2DiscoveryTest : public testing::Test {
 protected:
  void SetUp() override {
    // Stay below Linux's ephemeral-port range and give concurrent test
    // processes distinct topics even if their domain IDs happen to coincide.
    domain_ = 80 + static_cast<uint32_t>(::getpid() % 30);
    topic_ = "/ibmw_discovery_test_" + std::to_string(::getpid());
    context_ = std::make_shared<rclcpp::Context>();
    rclcpp::InitOptions init;
    init.set_domain_id(domain_);
    init.auto_initialize_logging(false);
    context_->init(0, nullptr, init);
    rclcpp::NodeOptions options;
    options.context(context_).enable_rosout(false).start_parameter_services(false)
        .start_parameter_event_publisher(false).use_clock_thread(false)
        .append_parameter_override("start_type_description_service", false);
    node_ = std::make_shared<rclcpp::Node>("discovery_fixture", "/regression", options);
  }
  void TearDown() override {
    node_.reset();
    if (context_ && context_->is_valid()) context_->shutdown("test complete");
    context_.reset();
  }
  DiscoverySnapshot WaitForTopic(DiscoveryProvider& provider, size_t count) {
    const auto deadline = Clock::now() + 6s;
    DiscoverySnapshot snapshot;
    do {
      snapshot = provider.Query(Clock::now() + 100ms);
      const auto found = std::count_if(snapshot.endpoints.begin(), snapshot.endpoints.end(),
          [&](const auto& endpoint) { return endpoint.topic_name == topic_; });
      // Endpoint discovery and ROS node association arrive independently.
      const auto named = std::count_if(snapshot.endpoints.begin(), snapshot.endpoints.end(),
          [&](const auto& endpoint) {
            return endpoint.topic_name == topic_ && endpoint.node_name == "discovery_fixture";
          });
      if (static_cast<size_t>(found) == count && found == named) break;
    } while (Clock::now() < deadline);
    return snapshot;
  }

  uint32_t domain_;
  std::string topic_;
  rclcpp::Context::SharedPtr context_;
  rclcpp::Node::SharedPtr node_;
};

TEST_F(Ros2DiscoveryTest, ReportsGraphMetadataWithoutPublishingBusinessData) {
  auto publisher = node_->create_publisher<Message>(topic_, rclcpp::QoS(7).reliable().transient_local());
  auto subscriber = node_->create_subscription<Message>(topic_, rclcpp::QoS(7).reliable().transient_local(),
      [](Message::ConstSharedPtr) {});
  auto hidden = node_->create_publisher<Message>(topic_ + "/_hidden", 1);
  auto provider = MakeProvider();
  provider->Start({.backend = DiscoveryBackend::kRos2, .domain_id = domain_});
  const auto snapshot = WaitForTopic(*provider, 2);
  ASSERT_TRUE(snapshot.Succeeded());
  EXPECT_EQ(snapshot.sources.front().source.backend, DiscoveryBackend::kRos2);
  EXPECT_EQ(snapshot.sources.front().source.domain, std::to_string(domain_));
  std::set<std::string> ids;
  std::set<EndpointRole> roles;
  for (const auto& endpoint : snapshot.endpoints) {
    SCOPED_TRACE(endpoint.topic_name);
    EXPECT_NE(endpoint.node_name, "_ibmw_discovery");
    if (endpoint.topic_name != topic_) continue;
    EXPECT_EQ(endpoint.type_name, "rcl_interfaces/msg/ParameterEvent");
    EXPECT_EQ(endpoint.node_name, "discovery_fixture");
    EXPECT_EQ(endpoint.node_namespace, "/regression");
    EXPECT_FALSE(endpoint.hidden);
    EXPECT_TRUE(endpoint.participant_id.empty());
    EXPECT_TRUE(endpoint.wire_topic_name.empty());
    EXPECT_TRUE(endpoint.wire_type_name.empty());
    EXPECT_EQ(endpoint.endpoint_id.size(), RMW_GID_STORAGE_SIZE * 2);
    EXPECT_EQ(endpoint.qos.policies.at("reliability"), "reliable");
    EXPECT_EQ(endpoint.qos.policies.at("durability"), "transient_local");
    ids.insert(endpoint.endpoint_id);
    roles.insert(endpoint.role);
  }
  EXPECT_EQ(ids.size(), 2u);
  EXPECT_EQ(roles, (std::set{EndpointRole::kPublisher, EndpointRole::kSubscriber}));
  // Hidden topics are returned to the CLI with a flag, not discarded here.
  auto hidden_snapshot = provider->Query(Clock::now() + 200ms);
  EXPECT_TRUE(std::any_of(hidden_snapshot.endpoints.begin(), hidden_snapshot.endpoints.end(),
      [&](const auto& endpoint) { return endpoint.topic_name == topic_ + "/_hidden" && endpoint.hidden; }));

  publisher.reset();
  subscriber.reset();
  const auto after_removal = WaitForTopic(*provider, 0);
  EXPECT_TRUE(std::none_of(after_removal.endpoints.begin(), after_removal.endpoints.end(),
      [&](const auto& endpoint) { return endpoint.topic_name == topic_; }));
}

TEST_F(Ros2DiscoveryTest, IsolatesDomainsAndCanRestartWithoutShuttingDownOtherContexts) {
  auto publisher = node_->create_publisher<Message>(topic_, 1);
  auto provider = MakeProvider();
  provider->Stop();
  EXPECT_THROW(provider->Query(Clock::now()), std::logic_error);
  EXPECT_THROW(provider->Start({.backend = DiscoveryBackend::kDds}), std::invalid_argument);
  provider->Stop();
  provider->Start({.backend = DiscoveryBackend::kRos2, .domain_id = domain_ + 1});
  const auto isolated = provider->Query(Clock::now() + 1s);
  ASSERT_TRUE(isolated.Succeeded());
  EXPECT_TRUE(std::none_of(isolated.endpoints.begin(), isolated.endpoints.end(),
      [&](const auto& endpoint) { return endpoint.topic_name == topic_; }));
  provider->Stop();
  provider->Stop();
  EXPECT_TRUE(context_->is_valid());

  provider->Start({.backend = DiscoveryBackend::kRos2, .domain_id = domain_});
  EXPECT_THROW(provider->Start({.backend = DiscoveryBackend::kRos2, .domain_id = domain_}), std::logic_error);
  const auto matching = WaitForTopic(*provider, 1);
  EXPECT_TRUE(std::any_of(matching.endpoints.begin(), matching.endpoints.end(),
      [&](const auto& endpoint) { return endpoint.topic_name == topic_; }));
  provider.reset();
  EXPECT_TRUE(context_->is_valid());
}
}  // namespace
}  // namespace ibmw::discovery
