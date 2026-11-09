// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include <string>
#include <vector>

#include "cli/service/call_options.h"
#include "cli/service/call_type_support.h"
#include "cli/service/discovery_options.h"
#include "engine/discovery/service_discovery_protocol.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace ibmw::diag {
namespace {
namespace protocol = discovery::service_protocol;
namespace service = cli::service;
using Json = nlohmann::json;

TEST(DiagServiceDiscoveryTls, CredentialsRequireTlsMqttAndPairedClientFiles) {
  cli::Invocation invocation;
  EXPECT_TRUE(service::ParseDiscoveryOptions(invocation).front().mqtt_tls.Empty());
  invocation.options["--mqtt-client-cert"] = {__FILE__};
  EXPECT_THROW(service::ParseDiscoveryOptions(invocation), std::invalid_argument);
  invocation.options["--mqtt"] = {"ssl://localhost:8883"};
  EXPECT_THROW(service::ParseDiscoveryOptions(invocation), std::invalid_argument);
  invocation.options["--mqtt-client-key"] = {__FILE__};
  invocation.options["--mqtt-truststore"] = {__FILE__};
  const auto sources = service::ParseDiscoveryOptions(invocation);
  ASSERT_EQ(sources.size(), 1u);
  const auto& tls = sources.front().mqtt_tls;
  EXPECT_EQ(tls.truststore, __FILE__);
  EXPECT_EQ(tls.client_cert, __FILE__);
  EXPECT_EQ(tls.client_key, __FILE__);
  invocation.options["--mqtt"] = {"tcp://localhost:1883"};
  EXPECT_THROW(service::ParseDiscoveryOptions(invocation), std::invalid_argument);
  invocation.options["--mqtt"] = {"ssl://localhost:8883"};
  invocation.options["--mqtt-client-key"] = {"/does-not-exist/ibmw-key.pem"};
  EXPECT_THROW(service::ParseDiscoveryOptions(invocation), std::invalid_argument);
  invocation.options["--mqtt-client-key"] = {""};
  EXPECT_THROW(service::ParseDiscoveryOptions(invocation), std::invalid_argument);
  invocation.options["--mqtt-client-key"] = {__FILE__, __FILE__};
  EXPECT_THROW(service::ParseDiscoveryOptions(invocation), std::invalid_argument);
}

TEST(DiagServiceDiscoveryTls, CredentialsNeverBecomeDiscoveryIdentity) {
  cli::Invocation invocation;
  invocation.options = {{"--mqtt", {"ssl://localhost:8883"}},
                        {"--mqtt-truststore", {__FILE__}},
                        {"--mqtt-client-cert", {__FILE__}},
                        {"--mqtt-client-key", {__FILE__}}};
  invocation.options["--registry"] = {""};
  const auto sources = service::ParseDiscoveryOptions(invocation);
  ASSERT_EQ(sources.size(), 2u);
  EXPECT_EQ(sources[0].backend, discovery::ServiceDiscoveryBackend::kRegistry);
  EXPECT_TRUE(sources[0].mqtt_tls.Empty());
  EXPECT_EQ(sources[1].backend, discovery::ServiceDiscoveryBackend::kMqtt);
  EXPECT_EQ(sources[1].mqtt_tls.truststore, __FILE__);
  EXPECT_EQ(sources[1].mqtt_tls.client_cert, __FILE__);
  EXPECT_EQ(sources[1].mqtt_tls.client_key, __FILE__);
  EXPECT_EQ(discovery::DescribeSource(sources[1]).instance_id, "ssl://localhost:8883");
}

TEST(DiagServiceDirectory, CatalogDeduplicatesExactIdentityAndKeepsDistinctTypes) {
  protocol::Catalog catalog;
  catalog.Add("/sum", "Request", "Response");
  catalog.Add("/sum", "Request", "Response");
  catalog.Add("/sum", "OtherRequest", "Response");
  const auto wire = Json::parse(catalog.Encode());
  EXPECT_EQ(wire.size(), 3u);
  EXPECT_EQ(wire.at("services").size(), 2u);
  for (const auto& item : wire.at("services")) {
    EXPECT_EQ(item.size(), 3u);  // No registry paths, sockets or library paths.
    EXPECT_TRUE(item.contains("name") && item.contains("request_type") && item.contains("response_type"));
  }
  const discovery::ServiceDiscoverySource source{
      .backend = discovery::ServiceDiscoveryBackend::kHttp, .instance_id = "localhost:8080"};
  const auto snapshot = protocol::Decode(wire.dump(), source);
  ASSERT_EQ(snapshot.services.size(), 2u);
  EXPECT_TRUE(snapshot.Succeeded());
  EXPECT_EQ(snapshot.services[0].runtime_id, catalog.Instance());
  EXPECT_EQ(snapshot.services[0].source.instance_id, source.instance_id);
  EXPECT_EQ(snapshot.services[0].advertisement.available_drivers, (std::vector<std::string>{"http"}));
}

Json Directory() {
  return {{"version", 1}, {"instance", "server"}, {"services", Json::array({
      {{"name", "/calc"}, {"request_type", "Req"}, {"response_type", "Rsp"}}})}};
}

TEST(DiagServiceDirectory, RejectsMissingFieldsUnsupportedVersionsAndInvalidIdentity) {
  for (const auto* field : {"version", "instance", "services"}) {
    auto wire = Directory();
    wire.erase(field);
    EXPECT_THROW(protocol::Decode(wire.dump(), {}), std::exception) << field;
  }
  for (const Json& version : {Json(2), Json("1"), Json(1.0)}) {
    auto wire = Directory();
    wire["version"] = version;
    EXPECT_THROW(protocol::Decode(wire.dump(), {}), std::exception);
  }
  for (const auto* field : {"name", "request_type", "response_type"}) {
    for (const auto* invalid : {"", "line\nbreak", "line\rbreak"}) {
      auto wire = Directory();
      wire["services"][0][field] = invalid;
      EXPECT_THROW(protocol::Decode(wire.dump(), {}), std::exception) << field;
    }
  }
}

TEST(DiagServiceDirectory, DuplicateWireEntriesAndHiddenNamesAreHandled) {
  auto wire = Directory();
  wire["services"].push_back(wire["services"][0]);
  wire["services"].push_back({{"name", "/_internal/ping"}, {"request_type", "Req"}, {"response_type", "Rsp"}});
  const auto snapshot = protocol::Decode(wire.dump(), {});
  ASSERT_EQ(snapshot.services.size(), 2u);
  EXPECT_FALSE(snapshot.services[0].advertisement.hidden);
  EXPECT_TRUE(snapshot.services[1].advertisement.hidden);
  EXPECT_NE(snapshot.services[0].advertisement.endpoint_id, snapshot.services[1].advertisement.endpoint_id);
}

TEST(DiagServiceDirectory, SizeAndCountLimitsRejectOversizedSnapshots) {
  EXPECT_THROW(protocol::Decode(std::string(protocol::kMaxSnapshotBytes + 1, 'x'), {}), std::runtime_error);
  auto wire = Directory();
  const auto entry = wire["services"][0];
  wire["services"] = std::vector<Json>(protocol::kMaxServices + 1, entry);
  EXPECT_THROW(protocol::Decode(wire.dump(), {}), std::runtime_error);
  wire = Directory();
  wire["instance"] = std::string(257, 'x');
  EXPECT_THROW(protocol::Decode(wire.dump(), {}), std::runtime_error);
}

TEST(DiagServiceDirectory, CatalogRejectsIncompleteRegistration) {
  protocol::Catalog catalog;
  EXPECT_THROW(catalog.Add("", "Req", "Rsp"), std::invalid_argument);
  EXPECT_THROW(catalog.Add("/a", "", "Rsp"), std::invalid_argument);
  EXPECT_THROW(catalog.Add("/a", "Req", ""), std::invalid_argument);
  EXPECT_TRUE(protocol::Decode(catalog.Encode(), {}).services.empty());
}

TEST(DiagServiceFraming, BinaryPayloadRoundTripsAndMalformedFramesFail) {
  const std::string payload("abc\0def", 7);
  const auto frame = protocol::GrpcFrame(payload);
  EXPECT_EQ(protocol::GrpcPayload(frame), payload);
  EXPECT_TRUE(protocol::GrpcPayload(protocol::GrpcFrame("")).empty());
  EXPECT_THROW(protocol::GrpcPayload(frame.substr(1)), std::runtime_error);
  EXPECT_THROW(protocol::GrpcPayload(frame + "x"), std::runtime_error);
  auto compressed = frame;
  compressed[0] = 1;
  EXPECT_THROW(protocol::GrpcPayload(compressed), std::runtime_error);
  EXPECT_THROW(protocol::GrpcFrame(std::string(protocol::kMaxSnapshotBytes + 1, 'x')), std::runtime_error);
}

TEST(DiagServiceAddress, ParsesIpv4AndBracketedIpv6AndRejectsNonDirectoryUrls) {
  for (const auto* endpoint : {"host:80", "http://host:80/"}) {
    const auto address = protocol::ParseServerAddress(endpoint, "http");
    EXPECT_EQ(address.host, "host");
    EXPECT_EQ(address.port, "80");
  }
  EXPECT_EQ(protocol::ParseServerAddress("[::1]:8080", "http").host, "::1");
  for (const auto* endpoint : {"http://user@host:80", "host:0", "host:65536", "host:80/path", "https://host:80", "::1:8080"}) {
    EXPECT_THROW(protocol::ParseServerAddress(endpoint, "http"), std::invalid_argument) << endpoint;
  }
}

TEST(DiagServiceAddress, ZenohDomainEncodingCannotIntroduceWildcardPaths) {
  EXPECT_EQ(protocol::ZenohPrefix("a/b*"), "ibmw/discovery/services/v1/d612f622a/");
  EXPECT_NE(protocol::ZenohPrefix("a/b"), protocol::ZenohPrefix("ab"));
  EXPECT_TRUE(protocol::IsHidden("/_private/service"));
  EXPECT_FALSE(protocol::IsHidden("/public_name/service"));
}

cli::Invocation Call(std::string backend, std::string endpoint = "") {
  cli::Invocation in;
  in.positional_args = {"/diag/sum", "Req -> Rsp", "{a: 1}"};
  in.options["--" + backend] = {std::move(endpoint)};
  if (backend == "ros2") in.options["--ros-domain"] = {"0"};
  return in;
}

TEST(DiagServiceOptions, EachBackendBuildsOneClientAndDisablesRegistryAdvertisement) {
  for (const auto* backend : {"dds", "ros2", "http", "grpc", "mqtt", "zenoh"}) {
    SCOPED_TRACE(backend);
    auto in = Call(backend, std::string(backend) == "mqtt" ? "tcp://localhost:1883" : "localhost:8080");
    const auto options = service::ParseCallOptions(in);
    EXPECT_EQ(options.backend, backend);
    EXPECT_EQ(options.request, "{a: 1}");
    const auto runtime = options.root["ibmw"];
    EXPECT_FALSE(runtime["runtime_graph"]["enabled"].as<bool>());
    EXPECT_EQ(runtime["service"]["drivers"].size(), 1u);
    EXPECT_EQ(runtime["service"]["clients"][0]["enable_drivers"][0].as<std::string>(), backend);
  }
}

TEST(DiagServiceOptions, DeadlinesRemainIndependentAndTypePackagesCanRepeat) {
  auto in = Call("http", "localhost:8080");
  in.options["--timeout"] = {"0.25"};
  in.options["--discovery-timeout"] = {"1.5"};
  in.options["--type-support"] = {"a.so", "b.so"};
  const auto options = service::ParseCallOptions(in);
  EXPECT_EQ(options.timeout.count(), 250);
  EXPECT_EQ(options.discovery_timeout.count(), 1500);
  EXPECT_EQ(options.packages, (std::vector<std::string>{"a.so", "b.so"}));
  for (const auto* value : {"0", "nan", "inf", "3601", "1junk"}) {
    in.options["--timeout"] = {value};
    EXPECT_THROW(service::ParseCallOptions(in), std::invalid_argument) << value;
  }
}

TEST(DiagServiceOptions, TlsAndBackendSelectionAreValidatedBeforeAnyConnection) {
  auto in = Call("mqtt", "mqtts://localhost:8883");
  EXPECT_THROW(service::ParseCallOptions(in), std::invalid_argument);
  in.options["--mqtt-truststore"] = {"ca.pem"};
  EXPECT_EQ(service::ParseCallOptions(in).root["ibmw"]["extensions"]["items"][0]["options"]["broker_addr"].as<std::string>(), "ssl://localhost:8883");
  in.options["--mqtt-client-key"] = {"key.pem"};
  EXPECT_THROW(service::ParseCallOptions(in), std::invalid_argument);
  in = Call("http", "localhost:8080");
  in.options["--dds"] = {""};
  EXPECT_THROW(service::ParseCallOptions(in), std::invalid_argument);
}

TEST(DiagServicePayload, LiteralScalarsSurviveConversionAndAmbiguousDocumentsFail) {
  const auto value = Json::parse(service::ParseCallRequest("{label: '001', ok: true, n: -42, values: [null, 'false']}"));
  EXPECT_EQ(value["label"], "001");
  EXPECT_EQ(value["ok"], true);
  EXPECT_EQ(value["n"], -42);
  EXPECT_TRUE(value["values"][0].is_null());
  EXPECT_EQ(value["values"][1], "false");
  for (const auto* text : {"{x: 1, x: 2}", "x: 1\n---\nx: 2", "[1, 2]", "{x: .inf}"}) {
    EXPECT_THROW(service::ParseCallRequest(text), std::exception) << text;
  }
}
}  // namespace
}  // namespace ibmw::diag
