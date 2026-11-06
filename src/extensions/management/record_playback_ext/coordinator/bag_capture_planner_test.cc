// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/coordinator/bag_capture_planner.h"

#include "gtest/gtest.h"
#include "engine/runtime_graph/topic_graph.h"
#include "engine/transport/serialized_transport_driver.h"
#include "record_playback_ext/coordinator/dynamic_capture_controller.h"

namespace ibmw::extensions::record_playback_extension {
namespace {
namespace io = ibmw::runtime::core::topic_io;

TopicAdvertisement RegistryPublisher(std::string driver, std::string mode = "native_serialized", uint32_t priority = 1) {
  TopicAdvertisement publisher{
      .runtime_id = "runtime", .control_socket = "/tmp/runtime.sock", .publisher_id = "publisher",
      .topic = {.topic = "/topic", .msg_type = "dds:Message", .serialization_type = "cdr"},
      .type_support_package = "/tmp/type.so"};
  publisher.topic.schema.schema_hash = "schema";
  publisher.topic.available_drivers = {driver};
  publisher.capture_capabilities[driver] = {
      .abi_version = ibmw::runtime::core::transport::SerializedTransportCapabilityV2::kAbiVersion,
      .mode = mode, .priority = priority, .extension_name = driver + "_extension",
      .extension_path = "/tmp/driver.so", .extension_options_yaml = "{domain_id: 87, participant_name: publisher}",
      .driver_options_yaml = "{}", .provider_instance_option = "participant_name"};
  publisher.capture_capabilities["runtime-tap"].mode = "runtime_tap";
  return publisher;
}

CaptureGraph RegistryGraph(std::vector<TopicAdvertisement> publishers) {
  return BuildCaptureGraph({{.runtime_id = "runtime", .control_socket = "/tmp/runtime.sock",
                            .topics = std::move(publishers)}});
}

TEST(BagCaptureGraphTest, RegistryPrefersAllFiveIndependentTransports) {
  for (const std::string driver : {"dds", "ros2", "zenoh", "iceoryx", "mqtt"}) {
    SCOPED_TRACE(driver);
    const auto mode = driver == "iceoryx" || driver == "mqtt" ? "cached_serialized" : "native_serialized";
    const auto subscriptions = RegistryGraph({RegistryPublisher(driver, mode)}).Subscriptions("/topic");
    ASSERT_EQ(subscriptions.size(), 1u);
    EXPECT_EQ(subscriptions[0].driver, driver);
  }
}

TEST(BagCaptureGraphTest, UnsupportedDriversUseOneTapPerPublisherAndRequireCapability) {
  for (const std::string driver : {"local", "tcp", "udp", "http"}) {
    auto first = RegistryPublisher(driver, "runtime_tap");
    auto second = first;
    second.publisher_id = "other";
    auto subscriptions = RegistryGraph({first, second}).Subscriptions("/topic");
    ASSERT_EQ(subscriptions.size(), 2u);
    EXPECT_EQ(subscriptions[0].driver, "runtime-tap");
    EXPECT_EQ(subscriptions[1].driver, "runtime-tap");
    second.capture_capabilities.erase("runtime-tap");
    EXPECT_ANY_THROW(RegistryGraph({first, second}).Subscriptions("/topic"));
  }
}

TEST(BagCaptureGraphTest, NativeRouteDeduplicatesPublishersAndAdaptsQos) {
  auto first = RegistryPublisher("dds");
  first.topic.qos.policies = {{"reliability", "reliable"}, {"durability", "transient_local"}};
  auto second = first;
  second.publisher_id = "other";
  second.capture_capabilities["dds"].extension_options_yaml = "{participant_name: another, domain_id: 87}";
  second.driver_qos["dds"].policies = {{"reliability", "best_effort"}, {"durability", "volatile"}};
  const auto subscriptions = RegistryGraph({first, second}).Subscriptions("/topic");
  ASSERT_EQ(subscriptions.size(), 1u);
  EXPECT_EQ(subscriptions[0].driver, "dds");
  EXPECT_EQ(subscriptions[0].topic.qos.policies.at("reliability"), "best_effort");
  EXPECT_EQ(subscriptions[0].topic.qos.policies.at("durability"), "volatile");
  second.topic.qos = first.topic.qos;
  second.driver_qos.clear();
  EXPECT_TRUE(RegistryGraph({first}).Subscriptions("/topic")[0].SameSubscription(
      RegistryGraph({second}).Subscriptions("/topic")[0]));
}

TEST(BagCaptureGraphTest, ConflictingConnectionsAndPartialRoutesFallBackToTap) {
  auto first = RegistryPublisher("dds");
  auto second = first;
  second.publisher_id = "other";
  second.capture_capabilities["dds"].extension_options_yaml = "{domain_id: 88}";
  auto subscriptions = RegistryGraph({first, second}).Subscriptions("/topic");
  ASSERT_EQ(subscriptions.size(), 2u);
  EXPECT_EQ(subscriptions[0].driver, "runtime-tap");
  second.topic.available_drivers = {"local"};
  EXPECT_EQ(RegistryGraph({first, second}).Subscriptions("/topic").size(), 2u);
}

TEST(BagCaptureGraphTest, CachedSinglePublisherPriorityAndDynamicEligibility) {
  auto first = RegistryPublisher("iceoryx", "cached_serialized", 0);
  const auto dds = RegistryPublisher("dds");
  first.topic.available_drivers.push_back("dds");
  first.capture_capabilities["dds"] = dds.capture_capabilities.at("dds");
  EXPECT_EQ(RegistryGraph({first}).Subscriptions("/topic")[0].driver, "iceoryx");
  auto second = first;
  second.publisher_id = "other";
  EXPECT_EQ(RegistryGraph({first, second}).Subscriptions("/topic")[0].driver, "dds");
  const auto no_typed = [](const io::RegistrySubscription& subscription) { return subscription.driver != "iceoryx"; };
  EXPECT_EQ(RegistryGraph({first}).Subscriptions("/topic", no_typed)[0].driver, "dds");
  first.capture_capabilities.erase("dds");
  EXPECT_EQ(RegistryGraph({first}).Subscriptions("/topic", no_typed)[0].driver, "runtime-tap");
  first.type_support_package.clear();
  EXPECT_EQ(RegistryGraph({first}).Subscriptions("/topic")[0].driver, "runtime-tap");
}

// The planner routes by capability, not by driver name: a "shm" driver with the
// capability shm_extension registers (cached_serialized, priority 0, as
// iceoryx_extension does; see shm_extension.cc) gets the iceoryx route.
TEST(BagCaptureGraphTest, ShmNamedCachedDriverRoutesLikeIceoryx) {
  EXPECT_EQ(RegistryGraph({RegistryPublisher("shm", "cached_serialized", 0)}).Subscriptions("/topic")[0].driver, "shm");
  auto first = RegistryPublisher("shm", "cached_serialized", 0);
  const auto dds = RegistryPublisher("dds");
  first.topic.available_drivers.push_back("dds");
  first.capture_capabilities["dds"] = dds.capture_capabilities.at("dds");
  EXPECT_EQ(RegistryGraph({first}).Subscriptions("/topic")[0].driver, "shm");
  auto second = first;
  second.publisher_id = "other";
  EXPECT_EQ(RegistryGraph({first, second}).Subscriptions("/topic")[0].driver, "dds");
  const auto no_typed = [](const io::RegistrySubscription& subscription) { return subscription.driver != "shm"; };
  EXPECT_EQ(RegistryGraph({first}).Subscriptions("/topic", no_typed)[0].driver, "dds");
  first.capture_capabilities.erase("dds");
  second.capture_capabilities.erase("dds");
  EXPECT_EQ(RegistryGraph({first}).Subscriptions("/topic", no_typed)[0].driver, "runtime-tap");
  const auto taps = RegistryGraph({first, second}).Subscriptions("/topic");
  ASSERT_EQ(taps.size(), 2u);
  EXPECT_EQ(taps[0].driver, "runtime-tap");
  EXPECT_EQ(taps[1].driver, "runtime-tap");
  first.type_support_package.clear();
  EXPECT_EQ(RegistryGraph({first}).Subscriptions("/topic")[0].driver, "runtime-tap");
}

TEST(BagCaptureGraphTest, NativeAddressMappingAndIsolatedObserverConfiguration) {
  auto ros = RegistryPublisher("ros2");
  ros.topic.msg_type = "ros2:std_msgs/msg/String";
  const auto ros_options = io::RegistrySubscriberOptions({ros, "ros2"});
  EXPECT_EQ(ros_options.wire_topic_name, "/topic");
  EXPECT_EQ(ros_options.wire_type_name, "std_msgs/msg/String");
  auto zenoh = RegistryPublisher("zenoh");
  auto& capability = zenoh.capture_capabilities["zenoh"];
  capability.extension_options_yaml = "{limit_domain: /robot}";
  capability.provider_strip_leading_slash_option = "limit_domain";
  const auto zenoh_options = io::RegistrySubscriberOptions({zenoh, "zenoh"});
  EXPECT_EQ(zenoh_options.wire_topic_name, "transport/%2Ftopic/dds%3AMessage/robot");
  capability.extension_options_yaml = "{limit_domain: ''}";
  EXPECT_EQ(io::RegistrySubscriberOptions({zenoh, "zenoh"}).wire_topic_name,
            "transport/%2Ftopic/dds%3AMessage");
  EXPECT_FALSE(io::RegistrySubscriptionConfig({zenoh, "zenoh"})["ibmw"]["extensions"]["items"][0]["options"]["limit_domain"]);
  const auto config = io::RegistrySubscriptionConfig({ros, "ros2"})["ibmw"];
  EXPECT_FALSE(config["runtime_graph"]["enabled"].as<bool>());
  EXPECT_EQ(config["extensions"]["items"].size(), 1u);
  EXPECT_NE(config["extensions"]["items"][0]["options"]["participant_name"].as<std::string>(), "publisher");
}

class TrackingRegistrySource final : public SerializedMessageSource {
 public:
  TrackingRegistrySource(int& starts, int& stops) : starts_(starts), stops_(stops) {}
  void Start(ibmw::transport::SerializedMessageCallback) override { ++starts_; }
  void Stop() override { ++stops_; }
  std::string Description() const override { return "test"; }
 private:
  int& starts_;
  int& stops_;
};

TEST(BagCaptureGraphTest, DynamicReconcileChangesRouteWithoutDuplicatingReaders) {
  auto first = RegistryPublisher("mqtt", "cached_serialized", 3);
  auto second = first;
  second.publisher_id = "other";
  auto graph = RegistryGraph({first});
  int starts = 0, stops = 0;
  BagCapturePlanner planner(graph, [&](const auto&) { return std::make_unique<TrackingRegistrySource>(starts, stops); });
  DynamicCaptureController controller([](auto&&) {});
  const auto reconcile = [&] { controller.Reconcile({"/topic"}, graph, [&](auto topic) { return planner.BuildPlan(topic); }); };
  reconcile();
  EXPECT_EQ(starts, 1);
  graph = RegistryGraph({first, second});
  reconcile();
  EXPECT_EQ(starts, 3);
  EXPECT_EQ(stops, 1);
  reconcile();
  EXPECT_EQ(starts, 3);
  graph = RegistryGraph({first});
  reconcile();
  EXPECT_EQ(starts, 4);
  EXPECT_EQ(stops, 3);
  controller.StopAll();
}

TEST(BagCaptureGraphTest, SchemaLessEndpointsRemainVisibleOnlyInGeneralDiscovery) {
  ibmw::runtime::core::runtime_graph::RuntimeRegistration runtime{.runtime_id = "runtime"};
  TopicAdvertisement publisher{
      .runtime_id = "runtime",
      .publisher_id = "publisher",
      .topic = {.topic = "/topic", .msg_type = "message",
                .serialization_type = "json"},
      .published = true};
  publisher.topic.schema.schema_hash = "schema-hash";
  auto subscriber = publisher;
  subscriber.publisher_id = "subscriber";
  subscriber.published = false;
  subscriber.topic.schema = {};
  auto unknown = subscriber;
  unknown.topic.topic = "/unknown";
  unknown.published = true;
  runtime.topics = {publisher, subscriber, unknown};

  ibmw::runtime::core::runtime_graph::TopicGraph discovery;
  discovery.MergeRuntime(runtime);
  const auto capture = BuildCaptureGraph({runtime});
  EXPECT_EQ(discovery.Find("/topic").size(), 2u);
  EXPECT_EQ(discovery.Find("/unknown").size(), 1u);
  const auto endpoints = capture.Find("/topic");
  ASSERT_EQ(endpoints.size(), 1u);
  EXPECT_EQ(endpoints.front().registry_advertisement->publisher_id, "publisher");
  EXPECT_TRUE(capture.Find("/unknown").empty());
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
