// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/coordinator/record_discovery.h"

#include <fstream>
#include <unistd.h>

#include "engine/runtime_graph/runtime_graph_agent.h"
#include "record_playback_ext/coordinator/dynamic_capture_controller.h"
#include "gtest/gtest.h"

namespace ibmw::extensions::record_playback_extension {
namespace {
using ibmw::discovery::DiscoveredEndpoint;
using ibmw::discovery::DiscoveryBackend;
using ibmw::discovery::DiscoverySnapshot;
using ibmw::discovery::EndpointRole;

TEST(RecordDiscoveryOptionsTest, DefaultAndExplicitRegistryUseTheSameSource) {
  RecordDiscoveryOptions defaults;
  EXPECT_FALSE(defaults.Network());
  EXPECT_NO_THROW(defaults.Validate());

  const char* argv[] = {"--registry", "--registry-path", "/tmp/record-registry"};
  RecordDiscoveryOptions explicit_registry;
  for (int index = 0; index < 3; ++index) {
    ASSERT_TRUE(explicit_registry.ParseArgument(index, 3, argv));
  }
  EXPECT_NO_THROW(explicit_registry.Validate());
  EXPECT_EQ(explicit_registry.source.backend, defaults.source.backend);
  EXPECT_EQ(explicit_registry.source.backend, DiscoveryBackend::kRegistry);
  EXPECT_EQ(explicit_registry.source.registry_path, "/tmp/record-registry");
  EXPECT_FALSE(explicit_registry.Network());

  RecordDiscoveryOptions custom_registry;
  int index = 1;
  ASSERT_TRUE(custom_registry.ParseArgument(index, 3, argv));
  EXPECT_NO_THROW(custom_registry.Validate());
  EXPECT_EQ(custom_registry.source.backend, defaults.source.backend);
  EXPECT_EQ(custom_registry.source.registry_path, explicit_registry.source.registry_path);
}

DiscoveredEndpoint Publisher(std::string id = "writer") {
  return {.source = {.backend = DiscoveryBackend::kDds, .domain = "87"},
          .participant_id = "participant", .endpoint_id = std::move(id),
          .wire_topic_name = "/wire_native_topic", .wire_type_name = "example::Foo",
          .serialization_type = "cdr",
          .qos = {.policies = {{"reliability", "reliable"}, {"durability", "transient_local"}}}};
}

TEST(RecordDiscoveryRegression, UnresolvedSchemasRemainVisibleAndOnlySelectedTopicsFail) {
  runtime::core::runtime_graph::TypeSupportRegistry types;
  const auto graph = ResolveNetworkCaptureGraph({.endpoints = {Publisher()}}, types);
  const auto endpoints = graph.Find("/wire_native_topic");
  ASSERT_EQ(endpoints.size(), 1u);
  EXPECT_EQ(endpoints[0].topic.msg_type, "dds:example::Foo");
  EXPECT_FALSE(endpoints[0].registry_advertisement.has_value());
  EXPECT_NO_THROW(ValidateCaptureSchemas(graph, {"/unrelated"}));
  EXPECT_ANY_THROW(ValidateCaptureSchemas(graph, {"/wire_native_topic"}));
}

TEST(RecordDiscoveryRegression, ReaderQosCoversAllPublishersAndIgnoresSubscribers) {
  runtime::core::runtime_graph::TypeSupportRegistry types;
  auto reliable = Publisher();
  auto best_effort = Publisher("other-writer");
  best_effort.qos.policies = {{"reliability", "best_effort"}, {"durability", "volatile"}};
  auto subscriber = best_effort;
  subscriber.role = EndpointRole::kSubscriber;
  subscriber.endpoint_id = "reader";
  auto graph = ResolveNetworkCaptureGraph({.endpoints = {reliable, subscriber}}, types);
  EXPECT_EQ(graph.Find(reliable.wire_topic_name)[0].topic.qos.policies.at("reliability"), "reliable");
  graph = ResolveNetworkCaptureGraph({.endpoints = {reliable, best_effort, subscriber}}, types);
  for (const auto& endpoint : graph.Find(reliable.wire_topic_name)) {
    EXPECT_EQ(endpoint.topic.qos.policies.at("reliability"), "best_effort");
    EXPECT_EQ(endpoint.topic.qos.policies.at("durability"), "volatile");
  }
}

TEST(RecordDiscoveryRegression, Ros2AndZenohPreserveTheirNativeSubscriptionAddresses) {
  runtime::core::runtime_graph::TypeSupportRegistry types;
  const auto ros = ResolveNetworkCaptureGraph({.endpoints = {{
      .source = {.backend = DiscoveryBackend::kRos2}, .topic_name = "/chatter",
      .type_name = "std_msgs/msg/String", .serialization_type = "cdr"}}}, types);
  const auto& ros_endpoint = ros.Find("/chatter").front();
  EXPECT_EQ(ros_endpoint.wire_topic_name, "/chatter");
  EXPECT_EQ(ros_endpoint.wire_type_name, "std_msgs/msg/String");
  EXPECT_EQ(ros_endpoint.topic.msg_type, "ros2:std_msgs/msg/String");
  EXPECT_EQ(ros_endpoint.topic.serialization_type, "ros2");
  EXPECT_FALSE(ros_endpoint.registry_advertisement);

  const auto zenoh = ResolveNetworkCaptureGraph({.endpoints = {{
      .source = {.backend = DiscoveryBackend::kZenoh}, .topic_name = "/chatter",
      .type_name = "json:Message", .wire_topic_name = "transport/%2Fchatter/json%3AMessage/robot"}}}, types);
  const auto& zenoh_endpoint = zenoh.Find("/chatter").front();
  EXPECT_EQ(zenoh_endpoint.wire_topic_name, "transport/%2Fchatter/json%3AMessage/robot");
  EXPECT_EQ(zenoh_endpoint.wire_type_name, "json:Message");
  EXPECT_TRUE(zenoh_endpoint.topic.serialization_type.empty());
  EXPECT_FALSE(zenoh_endpoint.registry_advertisement);
}

TEST(RecordDiscoveryRegression, ConflictingPublisherTypesAreRejectedButSubscribersDoNotSelectCaptureType) {
  runtime::core::runtime_graph::TypeSupportRegistry types;
  auto first = Publisher();
  auto other = Publisher("other");
  other.wire_type_name = "example::Other";
  EXPECT_ANY_THROW(ResolveNetworkCaptureGraph({.endpoints = {first, other}}, types).Subscriptions(first.wire_topic_name));
  other.role = EndpointRole::kSubscriber;
  EXPECT_EQ(ResolveNetworkCaptureGraph({.endpoints = {first, other}}, types).Subscriptions(first.wire_topic_name).size(), 1u);
  other.role = EndpointRole::kPublisher;
  other.wire_type_name = first.wire_type_name;
  other.qos.policies["ownership"] = "exclusive";
  EXPECT_ANY_THROW(ResolveNetworkCaptureGraph({.endpoints = {first, other}}, types).Subscriptions(first.wire_topic_name));
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern = "/tmp/ibmw-record-discovery-test-XXXXXX";
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
    path = pattern;
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
  std::filesystem::path path;
};

TEST(RecordDiscoveryRegression, RegistryProviderPreservesCaptureDetailsAndDropsExitedRuntimes) {
  TemporaryDirectory temporary;
  runtime::core::runtime_graph::TypeSupportRegistry types;
  RecordDiscoveryOptions options;
  options.source.registry_path = temporary.path;
  RecordDiscovery discovery(options, types);
  {
    TopicAdvertisement topic{
        .publisher_id = "publisher",
        .topic = {.topic = "/local", .msg_type = "message", .serialization_type = "json"},
        .capture_capabilities = {{"runtime-tap", {.mode = "runtime_tap"}}}};
    topic.topic.schema = {.type_name = "message", .serialization_type = "json",
                         .schema_name = "message", .schema_encoding = "jsonschema",
                         .schema_data = "{}", .schema_hash = ibmw::util::ComputeSchemaHash("{}")};
    runtime::core::runtime_graph::RuntimeGraphAgent agent({
        .registration = {.runtime_id = "record-local", .topics = {topic}},
        .registry_path = temporary.path});
    agent.Initialize();
    const auto endpoints = discovery.Query().Find("/local");
    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_FALSE(endpoints[0].registry_advertisement->control_socket.empty());
    EXPECT_EQ(endpoints[0].topic.schema.schema_hash, topic.topic.schema.schema_hash);
    EXPECT_EQ(endpoints[0].registry_advertisement->capture_capabilities.at("runtime-tap").mode, "runtime_tap");
  }
  EXPECT_TRUE(discovery.Query().Topics().empty());
}

TEST(RecordDiscoveryRegression, RecorderConfigurationResolvesPathsAndRejectsDomainMismatch) {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "recorder.yaml";
  std::ofstream(path) << R"(ibmw:
  extensions:
    items:
      - name: dds_extension
        path: libdds.so
        options: {domain_id: 87}
  messaging:
    drivers: [{type: dds}]
)";
  runtime::core::runtime_graph::TypeSupportRegistry types;
  RecordDiscoveryOptions options;
  options.source.backend = DiscoveryBackend::kDds;
  options.config_file = path;
  const auto connection = LoadRecordConnection(options, types);
  EXPECT_EQ(options.source.domain_id, 87u);
  EXPECT_EQ(connection.runtime_config["ibmw"]["extensions"]["items"][0]["path"].as<std::string>(), (temporary.path / "libdds.so").string());
  options.domain = 88;
  EXPECT_ANY_THROW(LoadRecordConnection(options, types));
}

TEST(RecordDiscoveryRegression, Ros2ConfigurationOwnsDomainAndZenohCanSeparateObserverSession) {
  TemporaryDirectory temporary;
  runtime::core::runtime_graph::TypeSupportRegistry types;
  const auto path = temporary.path / "recorder.yaml";
  std::ofstream(path) << R"(ibmw:
  extensions:
    items:
      - name: ros2_extension
        path: libros2.so
        options: {domain_id: 87, node_name: publisher}
      - name: zenoh_extension
        path: libzenoh.so
        options: {limit_domain: robot, native_cfg_path: capture.json5}
  messaging:
    drivers: [{type: ros2}, {type: zenoh}]
)";
  RecordDiscoveryOptions options;
  options.config_file = path;
  options.source.backend = DiscoveryBackend::kRos2;
  const auto ros = LoadRecordConnection(options, types);
  EXPECT_EQ(options.source.domain_id, 87u);
  EXPECT_EQ(ros.driver, "ros2");
  const auto ros_extensions = ros.runtime_config["ibmw"]["extensions"]["items"];
  ASSERT_EQ(ros_extensions.size(), 1u);
  EXPECT_NE(ros_extensions[0]["options"]["node_name"].as<std::string>(), "publisher");
  options.source.backend = DiscoveryBackend::kZenoh;
  options.source.zenoh_config = temporary.path / "observer.json5";
  const auto zenoh = LoadRecordConnection(options, types);
  EXPECT_EQ(options.source.zenoh_domain, "robot");
  EXPECT_EQ(options.source.zenoh_config, temporary.path / "observer.json5");
  EXPECT_EQ(zenoh.runtime_config["ibmw"]["extensions"]["items"][0]["options"]["native_cfg_path"].as<std::string>(),
            (temporary.path / "capture.json5").string());
  options.zenoh_domain = "different";
  EXPECT_ANY_THROW(LoadRecordConnection(options, types));
}

class CountingSource final : public SerializedMessageSource {
 public:
  CountingSource(int& starts, int& stops) : starts_(starts), stops_(stops) {}
  void Start(ibmw::transport::SerializedMessageCallback) override { ++starts_; }
  void Stop() override { ++stops_; }
  std::string Description() const override { return "counting source"; }
 private:
  int& starts_;
  int& stops_;
};

TEST(RecordDiscoveryRegression, LateQosChangesRestartSubscriptionAndMultiplePublishersShareIt) {
  runtime::core::runtime_graph::TypeSupportRegistry types;
  auto first = Publisher();
  DiscoverySnapshot snapshot{.endpoints = {first}};
  CaptureGraph graph;
  const auto update = [&] {
    graph = ResolveNetworkCaptureGraph(snapshot, types);
  };
  int starts = 0;
  int stops = 0;
  BagCapturePlanner planner(graph, [&](const auto&) {
    return std::make_unique<CountingSource>(starts, stops);
  });
  DynamicCaptureController controller([](auto&&) {});
  const auto reconcile = [&] {
    update();
    controller.Reconcile({first.wire_topic_name}, graph, [&](auto name) {
      return planner.BuildPlan(name);
    });
  };
  reconcile();
  auto second = Publisher("writer-2");
  snapshot.endpoints.push_back(second);
  reconcile();
  EXPECT_EQ(starts, 1);
  snapshot.endpoints.erase(snapshot.endpoints.begin());
  reconcile();
  EXPECT_EQ(starts, 1);  // A reader is independent of a particular publisher ID.
  snapshot.endpoints[0].qos.policies["reliability"] = "best_effort";
  reconcile();
  EXPECT_EQ(starts, 2);
  EXPECT_EQ(stops, 1);
  snapshot.endpoints.clear();
  reconcile();
  EXPECT_EQ(controller.ActiveCount(), 0u);
  EXPECT_EQ(stops, 2);
  snapshot.endpoints = {second};
  reconcile();
  EXPECT_EQ(starts, 3);
  controller.StopAll();
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
