// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include <chrono>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cli/topic/bandwidth_statistics.h"
#include "cli/topic/discovery_options.h"
#include "cli/topic/discovery_query.h"
#include "cli/topic/echo_formatter.h"
#include "cli/topic/pub_options.h"
#include "cli/topic/rate_statistics.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace ibmw::diag {
namespace {
namespace topic = cli::topic;
using namespace std::chrono_literals;

TEST(DiagTopicDiscovery, LogicalNamesTakePrecedenceAndWireOnlyEndpointsRemainVisible) {
  discovery::DiscoverySnapshot snapshot;
  snapshot.endpoints = {
      {.topic_name = "/camera", .type_name = "Image", .wire_topic_name = "rt/camera", .wire_type_name = "WireImage"},
      {.topic_name = "/camera", .type_name = "Image"},
      {.topic_name = "/camera", .type_name = "CompressedImage"},
      {.wire_topic_name = "raw", .wire_type_name = "WireOnly"},
      {.topic_name = "/_control", .type_name = "Control", .hidden = true}};
  EXPECT_EQ(topic::VisibleTopicNames(snapshot, false), (std::vector<std::string>{"/camera", "raw"}));
  EXPECT_EQ(topic::VisibleTopicNames(snapshot, true).size(), 3u);
  EXPECT_EQ(topic::TopicTypes(topic::VisibleEndpoints(snapshot, "/camera", false)),
            (std::set<std::string>{"CompressedImage", "Image"}));
  EXPECT_EQ(topic::TopicTypes(topic::VisibleEndpoints(snapshot, "raw", false)),
            (std::set<std::string>{"WireOnly"}));
  EXPECT_TRUE(topic::VisibleEndpoints(snapshot, "rt/camera", false).empty());
}

TEST(DiagTopicDiscovery, MissingTypesAreNotInventedAndSnapshotsAreNotMutated) {
  discovery::DiscoverySnapshot snapshot;
  snapshot.endpoints = {{.topic_name = "/unknown"}};
  auto endpoints = topic::VisibleEndpoints(snapshot, "/unknown", false);
  EXPECT_TRUE(topic::TopicTypes(endpoints).empty());
  endpoints[0].type_name = "Changed";
  EXPECT_TRUE(snapshot.endpoints[0].type_name.empty());
  EXPECT_TRUE(topic::VisibleTopicNames({}, true).empty());
}

TEST(DiagTopicDiscovery, CombinedSourcesKeepExplicitDomainsAndRegistryPath) {
  cli::Invocation in;
  in.options = {{"--all", {""}}, {"--domain", {"19"}}, {"--dds-domain", {"23"}},
                {"--zenoh-domain", {"robot/a"}}, {"--registry-path", {"relative-registry"}}};
  const auto sources = topic::ParseDiscoveryOptions(in);
  ASSERT_EQ(sources.size(), 4u);
  EXPECT_EQ(sources[0].backend, discovery::DiscoveryBackend::kRegistry);
  EXPECT_EQ(sources[0].registry_path, "relative-registry");
  EXPECT_EQ(sources[1].domain_id, 23u);
  EXPECT_EQ(sources[2].domain_id, 19u);
  EXPECT_EQ(sources[3].zenoh_domain, "robot/a");
}

TEST(DiagTopicDiscovery, DomainBoundsAndIncompleteSnapshotsAreExplicit) {
  cli::Invocation in;
  in.options = {{"--dds", {""}}, {"--domain", {"4294967295"}}};
  EXPECT_EQ(topic::ParseDiscoveryOptions(in).front().domain_id, UINT32_MAX);
  in.options["--domain"] = {"4294967296"};
  EXPECT_THROW(topic::ParseDiscoveryOptions(in), std::invalid_argument);
  discovery::DiscoverySnapshot snapshot;
  EXPECT_FALSE(snapshot.Succeeded());
  snapshot.sources.push_back({.status = discovery::DiscoveryStatus::kReady});
  EXPECT_TRUE(snapshot.Succeeded());
  snapshot.sources.push_back({.status = discovery::DiscoveryStatus::kUnavailable});
  EXPECT_FALSE(snapshot.Succeeded());
}

TEST(DiagTopicRate, RollingWindowDoesNotResetLifetimeCount) {
  topic::RateStatistics stats(3);
  const auto start = topic::RateStatistics::Clock::time_point{};
  for (auto offset : {0ms, 100ms, 300ms, 600ms}) stats.Observe(start + offset);
  const auto result = stats.Snapshot();
  EXPECT_EQ(result.received, 4u);
  EXPECT_EQ(result.window, 3u);
  EXPECT_DOUBLE_EQ(result.rate, 4.0);
  EXPECT_DOUBLE_EQ(result.minimum, 0.2);
  EXPECT_DOUBLE_EQ(result.maximum, 0.3);
  EXPECT_NEAR(result.deviation, 0.05, 1e-12);
}

TEST(DiagTopicRate, ClockResetCanRecoverWithoutKeepingOldIntervals) {
  topic::RateStatistics stats(4);
  const auto start = topic::RateStatistics::Clock::time_point{};
  stats.Observe(start + 10s);
  stats.Observe(start + 11s);
  stats.Observe(start);
  EXPECT_EQ(stats.Snapshot().window, 1u);
  stats.Observe(start + 250ms);
  EXPECT_DOUBLE_EQ(stats.Snapshot().rate, 4.0);
  EXPECT_EQ(stats.Snapshot().received, 4u);
  EXPECT_THROW(topic::RateStatistics(1), std::invalid_argument);
}

TEST(DiagTopicBandwidth, SilenceLowersRateWithoutChangingPayloadStatistics) {
  topic::BandwidthStatistics stats(2);
  const auto start = topic::BandwidthStatistics::Clock::time_point{};
  stats.Observe(100, start);
  stats.Observe(300, start + 1s);
  EXPECT_DOUBLE_EQ(stats.Snapshot(start + 1s).bytes_per_second, 400.0);
  const auto silent = stats.Snapshot(start + 4s);
  EXPECT_TRUE(silent.ready);
  EXPECT_DOUBLE_EQ(silent.bytes_per_second, 100.0);
  EXPECT_DOUBLE_EQ(silent.mean, 200.0);
  EXPECT_EQ(silent.minimum, 100u);
  EXPECT_EQ(silent.maximum, 300u);
  stats.Observe(500, start + 5s);
  EXPECT_EQ(stats.Snapshot(start + 5s).received, 3u);
  EXPECT_DOUBLE_EQ(stats.Snapshot(start + 5s).bytes_per_second, 200.0);
}

TEST(DiagTopicBandwidth, NoRateBeforeSecondSampleOrWhenObservationTimeIsInFuture) {
  topic::BandwidthStatistics stats(2);
  const auto start = topic::BandwidthStatistics::Clock::time_point{};
  EXPECT_FALSE(stats.Snapshot(start).ready);
  stats.Observe(0, start);
  EXPECT_FALSE(stats.Snapshot(start + 1s).ready);
  stats.Observe(0, start + 2s);
  EXPECT_FALSE(stats.Snapshot(start + 1s).ready);
  EXPECT_TRUE(stats.Snapshot(start + 2s).ready);
  EXPECT_DOUBLE_EQ(stats.Snapshot(start + 2s).bytes_per_second, 0.0);
  EXPECT_THROW(topic::BandwidthStatistics(0), std::invalid_argument);
}

cli::Invocation Publication(std::string backend) {
  cli::Invocation in;
  in.positional_args = {"/diag", "json:Diag", "{}"};
  in.options["--" + backend] = {""};
  if (backend != "local") in.options["--transport-library"] = {"not-loaded-by-unit-test.so"};
  return in;
}

TEST(DiagTopicPublication, LocalConfigurationHasNoExtensionAndKeepsExplicitCount) {
  auto in = Publication("local");
  in.options["--times"] = {"5"};
  in.options["--rate"] = {"2.5"};
  const auto result = topic::ParsePublicationOptions(in);
  EXPECT_EQ(result.count, 5u);
  EXPECT_DOUBLE_EQ(result.rate, 2.5);
  EXPECT_EQ(result.topic, "/diag");
  const auto runtime = result.root["ibmw"];
  EXPECT_FALSE(runtime["extensions"]);
  EXPECT_EQ(runtime["messaging"]["publishers"][0]["enable_drivers"][0].as<std::string>(), "local");
}

TEST(DiagTopicPublication, DdsConfigurationCarriesExplicitQosAndDomain) {
  auto in = Publication("dds");
  in.options["--dds-domain"] = {"31"};
  in.options["--qos-reliability"] = {"best_effort"};
  in.options["--qos-depth"] = {"17"};
  in.options["--once"] = {""};
  const auto result = topic::ParsePublicationOptions(in);
  const auto runtime = result.root["ibmw"];
  EXPECT_EQ(runtime["extensions"]["items"][0]["options"]["domain_id"].as<unsigned>(), 31u);
  const auto qos = runtime["messaging"]["drivers"][0]["options"]["publishers"][0]["qos"];
  EXPECT_EQ(qos["reliability"].as<std::string>(), "best_effort");
  EXPECT_EQ(qos["depth"].as<int>(), 17);
  EXPECT_EQ(result.count, 1u);
}

TEST(DiagTopicPublication, NetworkTargetsAreRetainedAndInvalidAddressesRejected) {
  auto in = Publication("udp");
  in.options["--target"] = {"127.0.0.1:9010", "127.0.0.1:9011"};
  const auto result = topic::ParsePublicationOptions(in);
  const auto targets = result.root["ibmw"]["messaging"]["drivers"][0]["options"]["publishers"][0]["server_url_list"];
  ASSERT_EQ(targets.size(), 2u);
  EXPECT_EQ(targets[1].as<std::string>(), "127.0.0.1:9011");
  for (const auto* invalid : {"localhost:9010", "127.0.0.1:0", "127.0.0.1:65536"}) {
    in.options["--target"] = {invalid};
    EXPECT_THROW(topic::ParsePublicationOptions(in), std::invalid_argument) << invalid;
  }
}

TEST(DiagTopicPublication, TlsCredentialsAndBackendSpecificOptionsAreValidated) {
  auto in = Publication("mqtt");
  in.options["--broker"] = {"mqtts://localhost:8883"};
  EXPECT_THROW(topic::ParsePublicationOptions(in), std::invalid_argument);
  in.options["--mqtt-truststore"] = {"ca.pem"};
  auto result = topic::ParsePublicationOptions(in);
  EXPECT_EQ(result.root["ibmw"]["extensions"]["items"][0]["options"]["broker_addr"].as<std::string>(), "ssl://localhost:8883");
  in.options["--mqtt-client-cert"] = {"cert.pem"};
  EXPECT_THROW(topic::ParsePublicationOptions(in), std::invalid_argument);
  in = Publication("local");
  in.options["--qos-depth"] = {"10"};
  EXPECT_THROW(topic::ParsePublicationOptions(in), std::invalid_argument);
}

transport::SerializedMessage TextMessage(const std::string& bytes, std::string format) {
  // The caller owns bytes for the full synchronous Format call.
  return {.topic = {.topic = "/diag", .msg_type = "json:Diag", .serialization_type = std::move(format)},
          .payload = std::make_shared<util::BufferArrayView>(bytes.data(), bytes.size())};
}

TEST(DiagTopicEcho, SelectedFieldPreservesStringAndNullValues) {
  const std::string text = R"({"items":[{"value":"001"},{"value":null}]})";
  auto message = TextMessage(text, "json");
  EXPECT_EQ(topic::EchoFormatter("json", "items.0.value", false).Format(message), "\"001\"");
  EXPECT_EQ(topic::EchoFormatter("json", "items.1.value", false).Format(message), "null");
  EXPECT_THROW(topic::EchoFormatter("json", "items.-1", false).Format(message), std::runtime_error);
  EXPECT_EQ(message.topic.msg_type, "json:Diag");
}

TEST(DiagTopicEcho, RejectsAmbiguousYamlAndSupportsRawUnknownSerialization) {
  const std::string duplicate = "value: 1\nvalue: 2\n";
  EXPECT_THROW(topic::EchoFormatter("json", "", false).Format(TextMessage(duplicate, "yaml")), std::runtime_error);
  const std::string bytes("\0\x7f\xff", 3);
  EXPECT_EQ(topic::EchoFormatter("json", "", true).Format(TextMessage(bytes, "opaque")), "00 7f ff");
  EXPECT_THROW(topic::EchoFormatter("json", "", false).Format({}), std::runtime_error);
}
}  // namespace
}  // namespace ibmw::diag
