// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/runtime_graph/runtime_graph_subsystem.h"

#include <fstream>
#include <future>
#include <unistd.h>

#include "engine/engine.h"
#include "engine/transport/serialized_transport_driver.h"
#include "extension_api/type_support_package_loader.h"
#include "gtest/gtest.h"

namespace ibmw::runtime::core::runtime_graph {
namespace {

TEST(RuntimeGraphSubsystemTest, FallbackOptionsAndExplicitPrecedence) {
  const ibmw::extension::RuntimeGraphOptions fallback{
      .enabled = false, .registry_path = "/tmp/fallback"};
  RuntimeGraphSubsystem legacy;
  legacy.Configure(YAML::Node{});
  legacy.ApplyFallbackOptions(fallback);
  EXPECT_FALSE(legacy.Enabled());
  EXPECT_EQ(legacy.RegistryPath(), "/tmp/fallback");
  RuntimeGraphSubsystem current;
  current.Configure(YAML::Load("{enabled: true, registry_path: /tmp/current}"));
  current.ApplyFallbackOptions(fallback);
  EXPECT_TRUE(current.Enabled());
  EXPECT_EQ(current.RegistryPath(), "/tmp/current");
}

TEST(RuntimeGraphSubsystemTest, LoadsFallbackTypeSupportAndAllowsSharedConfiguration) {
  const ibmw::extension::RuntimeGraphOptions fallback{
      .type_support_pkgs = {TYPE_SUPPORT_V1_FIXTURE_PATH}};
  RuntimeGraphSubsystem legacy;
  legacy.Configure(YAML::Node{});
  legacy.ApplyFallbackOptions(fallback);
  EXPECT_TRUE(legacy.FindTypeSupport("json:fixture.Message"));
  ASSERT_TRUE(legacy.FindSchema("json:fixture.Message", "json").has_value());
  EXPECT_FALSE(legacy.FindSchema("json:fixture.Message", "json")->schema_hash.empty());

  YAML::Node current_options;
  current_options["type_support_pkgs"].push_back(TYPE_SUPPORT_V1_FIXTURE_PATH);
  RuntimeGraphSubsystem current;
  current.Configure(current_options);
  EXPECT_NO_THROW(current.ApplyFallbackOptions(fallback));
  EXPECT_EQ(current.TypeSupportPackage("json:fixture.Message"),
            legacy.TypeSupportPackage("json:fixture.Message"));
}

TEST(RuntimeGraphSubsystemTest, ShutdownReleasesAllExtensionCallbacks) {
  RuntimeGraphSubsystem graph;
  auto owner = std::make_shared<int>(0);
  const std::weak_ptr<int> observer = owner;
  graph.SetStartTapHandler([owner](const auto&) { return TapSessionDescriptor{}; });
  graph.SetStopTapHandler([owner](auto) { return true; });
  graph.SetStartPlaybackHandler([owner](const auto&) { return TapSessionDescriptor{}; });
  graph.SetStopPlaybackHandler([owner](auto) { return true; });
  owner.reset();
  graph.Shutdown();
  EXPECT_TRUE(observer.expired());
}

class GraphTestDriver final : public transport::TransportDriver {
 public:
  std::string_view Name() const noexcept override { return "custom-capture"; }
  IBMW_DECLARE_DRIVER_SLOT("custom-capture")
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {}
  bool RegisterSendType(const transport::SendTypeWrapper&) noexcept override { return true; }
  bool OnMessage(const transport::MessageHandlerWrapper&) noexcept override { return true; }
  void Send(transport::MessageFrame&) noexcept override {}
};

class RuntimeGraphIntegrationTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = "/tmp/ibmw-graph-test-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    directory_ = pattern;
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }

  RuntimeRegistration Initialize(
      bool enable_tap = false,
      std::function<void(Engine&)> check = {}) {
    YAML::Node config = YAML::Load(R"(
ibmw:
  runtime_graph: {enabled: false}
  extensions:
    items:
      - name: extension-v2-fixture
        options: {fixture_option: retained}
  messaging:
    drivers:
      - type: custom-capture
        options: {driver_option: retained}
    publishers:
      - {topic: /test, enable_drivers: [custom-capture]}
)");
    config["ibmw"]["config"]["temp_cfg_path"] = (directory_ / "resolved").string();
    config["ibmw"]["extensions"]["items"][0]["path"] = EXTENSION_V2_FIXTURE_PATH;
    config["ibmw"]["runtime_graph"]["type_support_pkgs"].push_back(TYPE_SUPPORT_V1_FIXTURE_PATH);
    if (enable_tap) {
      config["ibmw"].remove("extensions");
      config["ibmw"]["runtime_graph"]["enabled"] = true;
      config["ibmw"]["runtime_graph"]["registry_path"] = directory_.string();
    }
    const auto path = directory_ / "config.yaml";
    std::ofstream(path) << config;
    ibmw::extension::TypeSupportPackageLoader loader;
    loader.Load(TYPE_SUPPORT_V1_FIXTURE_PATH);
    transport::SerializedTransportCapabilityV2 capability{
        .capture_mode = transport::SerializedCaptureMode::kCachedSerialized,
        .capture_priority = 17,
        .provider_name = "extension-v2-fixture",
        .provider_instance_option = "instance",
        .provider_clear_option = "clear",
        .provider_strip_leading_slash_option = "strip"};
    Engine engine;
    engine.RegisterHook(Before(Phase::kInitialize, Subsystem::kTransport), [&] {
      engine.GetTransportBus().RegisterTransportDriver(
          std::make_unique<GraphTestDriver>(),
          transport::TransportDriverCapabilitiesV1{}, capability);
    });
    engine.RegisterHook(Before(Phase::kInitialize, Subsystem::kModule), [&] {
      transport::SendTypeWrapper publisher;
      publisher.info = {.msg_type = "json:fixture.Message", .topic_name = "/test",
                        .pkg_path = "core", .module_name = "test",
                        .msg_type_support_ref = ibmw::util::TypeSupportRef(loader.TypeSupports().front())};
      EXPECT_TRUE(engine.GetTransportBus().RegisterSendType(std::move(publisher)));
    });
    engine.Initialize({.cfg_file_path = path.string()});
    const auto registration = engine.GetRuntimeGraphSubsystem().Registration();
    if (check) check(engine);
    return registration;
  }

  std::filesystem::path directory_;
};

TEST_F(RuntimeGraphIntegrationTest, PreservesDriverCapabilityContract) {
  const auto registration = Initialize();
  ASSERT_EQ(registration.topics.size(), 1u);
  const auto& topic = registration.topics.front();
  ASSERT_FALSE(topic.topic.schema.schema_hash.empty());
  const auto& capability = topic.capture_capabilities.at("custom-capture");
  EXPECT_EQ(capability.abi_version, transport::SerializedTransportCapabilityV2::kAbiVersion);
  EXPECT_EQ(capability.priority, 17u);
  EXPECT_EQ(capability.mode, "cached_serialized");
  EXPECT_EQ(capability.extension_name, "extension-v2-fixture");
  EXPECT_EQ(capability.provider_instance_option, "instance");
  EXPECT_EQ(capability.provider_clear_option, "clear");
  EXPECT_EQ(capability.provider_strip_leading_slash_option, "strip");
  EXPECT_NE(capability.extension_options_yaml.find("fixture_option"), std::string::npos);
  EXPECT_NE(capability.driver_options_yaml.find("driver_option"), std::string::npos);
  EXPECT_FALSE(topic.capture_capabilities.contains("runtime-tap"));
}

TEST_F(RuntimeGraphIntegrationTest, ReceivesTapWithoutRecordExtension) {
  const auto registration = Initialize(true, [](Engine& engine) {
    const auto* publisher = engine.GetTransportBus().GetTransportCatalog()
        ->GetSendTypeWrapperPtr("json:fixture.Message", "/test", "core", "test");
    ASSERT_NE(publisher, nullptr);
    auto stopped = engine.AsyncStart();
    const auto& registration = engine.GetRuntimeGraphSubsystem().Registration();
    ASSERT_EQ(registration.topics.size(), 1u);
    const auto& topic = registration.topics.front();
    ASSERT_TRUE(topic.capture_capabilities.contains("runtime-tap"));

    std::promise<ibmw::transport::SerializedMessage> received;
    auto message = received.get_future();
    topic_io::UnixRuntimeTapClient client(registration.control_socket);
    client.StartTap(topic, [&received](auto&& value) {
      received.set_value(std::move(value));
    });

    const std::string payload = "{\"value\":7}";
    ibmw::transport::Context context;
    transport::MessageFrame frame{
        .info = publisher->info,
        .ctx_ref = ibmw::transport::ContextRef(context)};
    frame.serialization_cache.emplace("json",
        std::make_shared<ibmw::util::BufferArrayView>(payload.data(), payload.size()));
    engine.GetTransportBus().Send(std::move(frame));

    const auto ready = message.wait_for(std::chrono::seconds(2));
    EXPECT_EQ(ready, std::future_status::ready);
    if (ready == std::future_status::ready) {
      const auto value = message.get();
      EXPECT_EQ(value.topic.topic, "/test");
      EXPECT_EQ(value.payload->JoinToString(), payload);
    }
    EXPECT_NO_THROW(client.StopTap());
    engine.Shutdown();
    EXPECT_EQ(stopped.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  });
  EXPECT_FALSE(std::filesystem::exists(registration.control_socket));
  EXPECT_FALSE(std::filesystem::exists(
      ibmw::extension::RuntimeRegistryDirectory(directory_) /
      (registration.runtime_id + ".json")));
}

TEST_F(RuntimeGraphIntegrationTest, ReceivesLargeTapPayloadIntact) {
  const auto registration = Initialize(true, [](Engine& engine) {
    const auto* publisher = engine.GetTransportBus().GetTransportCatalog()
        ->GetSendTypeWrapperPtr("json:fixture.Message", "/test", "core", "test");
    ASSERT_NE(publisher, nullptr);
    auto stopped = engine.AsyncStart();
    const auto& registration = engine.GetRuntimeGraphSubsystem().Registration();
    ASSERT_EQ(registration.topics.size(), 1u);
    const auto& topic = registration.topics.front();
    ASSERT_TRUE(topic.capture_capabilities.contains("runtime-tap"));

    std::promise<ibmw::transport::SerializedMessage> received;
    auto message = received.get_future();
    topic_io::UnixRuntimeTapClient client(registration.control_socket);
    client.StartTap(topic, [&received](auto&& value) {
      received.set_value(std::move(value));
    });

    // COMM-008: above the eager-allocation threshold the reader grows the
    // buffer as bytes arrive; the payload must still arrive intact.
    std::string payload(9 * 1024 * 1024 + 123, '\0');
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>('a' + i % 26);
    ibmw::transport::Context context;
    transport::MessageFrame frame{
        .info = publisher->info,
        .ctx_ref = ibmw::transport::ContextRef(context)};
    frame.serialization_cache.emplace("json",
        std::make_shared<ibmw::util::BufferArrayView>(payload.data(), payload.size()));
    engine.GetTransportBus().Send(std::move(frame));

    const auto ready = message.wait_for(std::chrono::seconds(10));
    EXPECT_EQ(ready, std::future_status::ready);
    if (ready == std::future_status::ready) {
      const auto value = message.get();
      EXPECT_EQ(value.topic.topic, "/test");
      EXPECT_TRUE(value.payload->JoinToString() == payload);
    }
    EXPECT_NO_THROW(client.StopTap());
    engine.Shutdown();
    EXPECT_EQ(stopped.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  });
  EXPECT_FALSE(std::filesystem::exists(registration.control_socket));
  EXPECT_FALSE(std::filesystem::exists(
      ibmw::extension::RuntimeRegistryDirectory(directory_) /
      (registration.runtime_id + ".json")));
}

}  // namespace
}  // namespace ibmw::runtime::core::runtime_graph
