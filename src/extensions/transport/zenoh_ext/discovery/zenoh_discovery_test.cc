// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "zenoh_discovery_protocol.h"
#include "extension_api/ibmw_discovery_provider.h"
#include "zenoh_ext/zenoh_transport_driver.h"
#include "zenoh_ext/zenoh_serialized_message.h"
#include "engine/topic_io/topic_subscription.h"
#include <future>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include <json/json.h>

namespace ibmw::discovery {
namespace {
using namespace std::chrono_literals;
namespace transport = ibmw::runtime::core::transport;
using ibmw::extensions::zenoh_extension::ZenohBus;
using ibmw::extensions::zenoh_extension::ZenohTransportDriver;
using Clock = std::chrono::steady_clock;

auto MakeProvider() {
  const auto api = IbmwDiscoveryEntryV1();
  return std::unique_ptr<DiscoveryProvider, decltype(api.destroy)>(api.create(), api.destroy);
}

TEST(ZenohDiscoveryProtocolTest, PreservesLogicalNamesAndUnknownFields) {
  const std::string domain = "/robot/*/%/机器人";
  const DiscoverySource source{.backend = DiscoveryBackend::kZenoh, .domain = domain};
  zenoh_protocol::Snapshot snapshot{.instance = "abc123", .domain = domain, .revision = 7,
      .endpoints = {{.endpoint_id = "1", .role = EndpointRole::kSubscriber,
          .topic_name = "/sensors/_hidden", .type_name = "proto:example.Point",
          .wire_topic_name = "transport/encoded/topic"}}};
  const auto key = zenoh_protocol::SnapshotKey(domain, snapshot.instance);
  EXPECT_EQ(key.find('*'), std::string::npos);
  EXPECT_NE(zenoh_protocol::DomainPrefix(""), zenoh_protocol::DomainPrefix("/"));
  EXPECT_NE(zenoh_protocol::DomainPrefix("a/b"), zenoh_protocol::DomainPrefix("a%2Fb"));
  const auto result = zenoh_protocol::Decode(zenoh_protocol::Encode(snapshot), key, source);
  ASSERT_EQ(result.endpoints.size(), 1u);
  const auto& endpoint = result.endpoints.front();
  EXPECT_EQ(result.revision, 7u);
  EXPECT_EQ(endpoint.topic_name, "/sensors/_hidden");
  EXPECT_EQ(endpoint.type_name, "proto:example.Point");
  EXPECT_EQ(endpoint.participant_id, "abc123");
  EXPECT_TRUE(endpoint.hidden);
  EXPECT_TRUE(endpoint.serialization_type.empty());
  EXPECT_TRUE(endpoint.wire_type_name.empty());
  EXPECT_TRUE(endpoint.qos.Empty());
}

TEST(ZenohDiscoveryProtocolTest, RejectsMalformedOrMismatchedSnapshots) {
  const DiscoverySource source{.backend = DiscoveryBackend::kZenoh, .domain = "robot"};
  zenoh_protocol::Snapshot snapshot{.instance = "abc", .domain = "robot", .revision = 1,
      .endpoints = {{.endpoint_id = "1", .topic_name = "/a", .type_name = "T", .wire_topic_name = "a"}}};
  const auto key = zenoh_protocol::SnapshotKey("robot", "abc");
  const auto encoded = zenoh_protocol::Encode(snapshot);
  EXPECT_THROW(zenoh_protocol::Decode("{}", key, source), std::runtime_error);
  EXPECT_THROW(zenoh_protocol::Decode(encoded, zenoh_protocol::SnapshotKey("robot", "def"), source), std::runtime_error);
  EXPECT_THROW(zenoh_protocol::Decode(encoded, key, {.backend = DiscoveryBackend::kZenoh, .domain = "else"}), std::runtime_error);
  EXPECT_THROW(zenoh_protocol::Decode(std::string(zenoh_protocol::kMaxSnapshotBytes + 1, ' '), key, source), std::runtime_error);
  Json::Value json;
  Json::CharReaderBuilder reader;
  std::string error;
  std::istringstream input(encoded);
  ASSERT_TRUE(Json::parseFromStream(reader, input, &json, &error));
  Json::StreamWriterBuilder writer;
  json["version"] = 2;
  EXPECT_THROW(zenoh_protocol::Decode(Json::writeString(writer, json), key, source), std::runtime_error);
  json["version"] = 1;
  json["endpoints"][0]["role"] = "unknown";
  EXPECT_THROW(zenoh_protocol::Decode(Json::writeString(writer, json), key, source), std::runtime_error);
  snapshot.endpoints.push_back(snapshot.endpoints.front());
  EXPECT_THROW(zenoh_protocol::Decode(zenoh_protocol::Encode(snapshot), key, source), std::runtime_error);
}

TEST(ZenohDiscoveryProtocolTest, RejectsWildcardNulAndNoncanonicalWireKeys) {
  const DiscoverySource source{.backend = DiscoveryBackend::kZenoh, .domain = "robot"};
  zenoh_protocol::Snapshot snapshot{.instance = "abc", .domain = "robot", .revision = 1,
      .endpoints = {{.endpoint_id = "1", .topic_name = "/a", .type_name = "T"}}};
  const auto reply_key = zenoh_protocol::SnapshotKey("robot", "abc");
  const std::vector<std::string> invalid{
      "", "transport/*", "transport/**", "transport/$*", "/transport/a",
      "transport/a/", "transport//a", std::string("transport/a\0b", 13)};
  for (const auto& key : invalid) {
    snapshot.endpoints.front().wire_topic_name = key;
    EXPECT_FALSE(zenoh_protocol::IsConcreteKey(key));
    EXPECT_THROW(zenoh_protocol::Decode(zenoh_protocol::Encode(snapshot), reply_key, source),
                 std::runtime_error);
  }
  snapshot.endpoints.front().wire_topic_name = "transport/exact";
  ASSERT_TRUE(zenoh_protocol::IsConcreteKey("transport/exact"));
  EXPECT_NO_THROW(zenoh_protocol::Decode(zenoh_protocol::Encode(snapshot), reply_key, source));
}

// Exercises the production registration hooks. Message types are deliberately
// opaque: discovery must work without sending or deserializing business data.
struct Business {
  std::vector<std::unique_ptr<transport::SendTypeWrapper>> publishers;
  std::vector<std::unique_ptr<transport::MessageHandlerWrapper>> subscribers;
  std::shared_ptr<ZenohBus> bus = std::make_shared<ZenohBus>();
  ZenohTransportDriver driver;

  Business(const std::string& config, const std::string& domain)
      : driver(bus, domain, 1024) {
    bus->Initialize(config, 1024 * 1024);
    driver.Initialize(YAML::Node(YAML::NodeType::Map));
  }
  ~Business() { driver.Shutdown(); bus->Shutdown(); }
  bool Add(const std::string& topic, EndpointRole role) {
    if (role == EndpointRole::kPublisher) {
      auto wrapper = std::make_unique<transport::SendTypeWrapper>();
      wrapper->info.topic_name = topic;
      wrapper->info.msg_type = "test:Opaque";
      const bool ok = driver.RegisterSendType(*wrapper);
      publishers.push_back(std::move(wrapper));
      return ok;
    }
    auto wrapper = std::make_unique<transport::MessageHandlerWrapper>();
    wrapper->info.topic_name = topic;
    wrapper->info.msg_type = "test:Opaque";
    const bool ok = driver.OnMessage(*wrapper);
    subscribers.push_back(std::move(wrapper));
    return ok;
  }
};

class ZenohDiscoveryTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = "/tmp/ibmw-zenoh-discovery-test-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    directory_ = pattern;
    // Reserve an ephemeral loopback port while preparing isolated peer configs.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    socklen_t size = sizeof(address);
    ASSERT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size), 0);
    const auto endpoint = "tcp/127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    server_ = WriteConfig("server.json5", endpoint, "");
    client_ = WriteConfig("client.json5", "tcp/127.0.0.1:0", endpoint);
    ::close(fd);
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }
  std::string WriteConfig(const char* name, const std::string& listen, const std::string& connect) {
    Json::Value config;
    config["mode"] = "peer";
    config["scouting"]["multicast"]["enabled"] = false;
    // Peers exchange loopback locators to form a full mesh. A peer with gossip
    // disabled does not act as a router between its connected peers.
    config["scouting"]["gossip"]["enabled"] = true;
    config["listen"]["endpoints"].append(listen);
    if (!connect.empty()) config["connect"]["endpoints"].append(connect);
    const auto path = directory_ / name;
    std::ofstream(path) << config;
    return path.string();
  }
  DiscoverySnapshot WaitFor(DiscoveryProvider& provider, size_t count) {
    const auto deadline = Clock::now() + 6s;
    DiscoverySnapshot snapshot;
    do {
      snapshot = provider.Query(Clock::now() + 100ms);
      if (snapshot.endpoints.size() == count) break;
    } while (Clock::now() < deadline);
    return snapshot;
  }
  std::filesystem::path directory_;
  std::string server_, client_;
};

TEST_F(ZenohDiscoveryTest, DiscoversSilentEndpointsFromAllInstancesAndKeepsDomainsSeparate) {
  Business first(server_, "robot/a");
  ASSERT_TRUE(first.Add("/shared", EndpointRole::kPublisher));
  ASSERT_TRUE(first.Add("/shared", EndpointRole::kPublisher));
  ASSERT_TRUE(first.Add("/shared", EndpointRole::kSubscriber));
  ASSERT_TRUE(first.Add("/shared", EndpointRole::kSubscriber));
  ASSERT_TRUE(first.Add("/only_subscriber", EndpointRole::kSubscriber));
  ASSERT_TRUE(first.Add("/_hidden", EndpointRole::kPublisher));
  first.driver.Start();
  // Surrounding slashes name the same domain (TRANS-047).
  Business second(client_, "/robot/a/");
  ASSERT_TRUE(second.Add("/shared", EndpointRole::kPublisher));
  second.driver.Start();
  Business other(client_, "robot/b");
  ASSERT_TRUE(other.Add("/other_domain", EndpointRole::kPublisher));
  other.driver.Start();

  auto provider = MakeProvider();
  provider->Start({.backend = DiscoveryBackend::kZenoh, .zenoh_domain = "robot/a", .zenoh_config = client_});
  // Collection starts before Query, as required when another backend consumes
  // the common window first.
  std::this_thread::sleep_for(500ms);
  const auto snapshot = WaitFor(*provider, 5);
  ASSERT_TRUE(snapshot.Succeeded());
  ASSERT_EQ(snapshot.endpoints.size(), 5u);
  std::set<std::string> instances, identities;
  for (const auto& endpoint : snapshot.endpoints) {
    instances.insert(endpoint.participant_id);
    identities.insert(endpoint.participant_id + '/' + endpoint.endpoint_id);
    EXPECT_EQ(endpoint.source.domain, "robot/a");
    EXPECT_EQ(endpoint.type_name, "test:Opaque");
    EXPECT_TRUE(endpoint.serialization_type.empty());
    EXPECT_TRUE(endpoint.qos.Empty());
    EXPECT_NE(endpoint.topic_name, "/other_domain");
    EXPECT_EQ(endpoint.hidden, endpoint.topic_name == "/_hidden");
    EXPECT_TRUE(endpoint.wire_topic_name.ends_with("/robot/a"));
    EXPECT_EQ(endpoint.wire_topic_name.find("//"), std::string::npos) << endpoint.wire_topic_name;
  }
  EXPECT_EQ(instances.size(), 2u);
  EXPECT_EQ(identities.size(), 5u);
  EXPECT_TRUE(std::any_of(snapshot.endpoints.begin(), snapshot.endpoints.end(), [](const auto& endpoint) {
    return endpoint.topic_name == "/only_subscriber" && endpoint.role == EndpointRole::kSubscriber;
  }));
}

TEST_F(ZenohDiscoveryTest, HandlesEmptyDomainRestartAndFailedInitialization) {
  Business business(server_, "");
  ASSERT_TRUE(business.Add("/empty_domain", EndpointRole::kPublisher));
  business.driver.Start();
  auto provider = MakeProvider();
  provider->Stop();
  EXPECT_THROW(provider->Query(Clock::now()), std::logic_error);
  EXPECT_THROW(provider->Start({.backend = DiscoveryBackend::kDds}), std::invalid_argument);
  EXPECT_THROW(provider->Start({.backend = DiscoveryBackend::kZenoh, .zenoh_config = directory_ / "missing"}), std::runtime_error);
  provider->Stop();
  provider->Start({.backend = DiscoveryBackend::kZenoh, .zenoh_config = client_});
  const auto found = WaitFor(*provider, 1);
  ASSERT_EQ(found.endpoints.size(), 1u);
  // The empty domain adds no key suffix, so the key does not end with '/'.
  EXPECT_FALSE(found.endpoints.front().wire_topic_name.ends_with('/')) << found.endpoints.front().wire_topic_name;
  provider->Stop();
  provider->Stop();
  // Withdrawal is observable without terminating the business session.
  business.driver.Shutdown();
  provider->Start({.backend = DiscoveryBackend::kZenoh, .zenoh_config = client_});
  const auto empty = provider->Query(Clock::now() + 500ms);
  EXPECT_TRUE(empty.Succeeded());
  EXPECT_TRUE(empty.endpoints.empty());
}

TEST_F(ZenohDiscoveryTest, WithdrawsExpiredInstancesWithoutRestartingObserver) {
  Business business(server_, "robot");
  ASSERT_TRUE(business.Add("/gone", EndpointRole::kPublisher));
  business.driver.Start();
  auto provider = MakeProvider();
  provider->Start({.backend = DiscoveryBackend::kZenoh, .zenoh_domain = "robot", .zenoh_config = client_});
  ASSERT_EQ(WaitFor(*provider, 1).endpoints.size(), 1u);
  business.driver.Shutdown();
  EXPECT_TRUE(WaitFor(*provider, 0).endpoints.empty());
}

std::string Envelope(std::string_view format, std::string_view payload) {
  std::string bytes(4 + 1 + format.size() + 1 + payload.size(), '\0');
  ibmw::common::util::BufferOperator buffer(bytes.data(), bytes.size());
  buffer.SetUint32(bytes.size() - 4);
  buffer.SetString(format, ibmw::common::util::BufferLenType::kUInt8);
  buffer.SetUint8(0);
  buffer.SetBuffer(payload.data(), payload.size());
  return bytes;
}

TEST_F(ZenohDiscoveryTest, TopicObserverReceivesOncePerWireSampleAndStops) {
  Business sender(server_, "hz_test");
  ASSERT_TRUE(sender.Add("/rate", EndpointRole::kPublisher));
  ASSERT_TRUE(sender.Add("/rate", EndpointRole::kPublisher));
  sender.driver.Start();
  ibmw::runtime::core::topic_io::TopicSubscription observer({
      .source = {.backend = DiscoveryBackend::kZenoh, .zenoh_domain = "hz_test", .zenoh_config = client_},
      .topic = "/rate"});
  std::atomic<unsigned> count{0};
  observer.Start([&](auto&& message) {
    EXPECT_EQ(message.payload->JoinToString(), "sample");
    ++count;
  });
  const auto discovery_deadline = Clock::now() + 5s;
  bool subscribed = false;
  while (!(subscribed = observer.Refresh()) && Clock::now() < discovery_deadline) {
    std::this_thread::sleep_for(50ms);
  }
  ASSERT_TRUE(subscribed);
  auto bytes = Envelope("json", "sample");
  const auto key = "transport/%2Frate/test%3AOpaque/hz_test";
  const auto deadline = Clock::now() + 3s;
  unsigned sent = 0;
  while (count.load() < 5 && Clock::now() < deadline) {
    ASSERT_TRUE(observer.Refresh());
    sender.bus->Send(key, bytes.data(), bytes.size());
    ++sent;
    std::this_thread::sleep_for(50ms);
  }
  observer.Stop();
  const auto stopped = count.load();
  EXPECT_GE(stopped, 5u);
  EXPECT_LE(stopped, sent);
  sender.bus->Send(key, bytes.data(), bytes.size());
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(count.load(), stopped);
}

TEST(ZenohKeyExprTest, NormalizesLimitDomain) {
  using ibmw::extensions::zenoh_extension::LimitDomainKeySuffix;
  using ibmw::extensions::zenoh_extension::NormalizeLimitDomain;
  EXPECT_EQ(NormalizeLimitDomain(""), "");
  EXPECT_EQ(NormalizeLimitDomain("//"), "");
  EXPECT_EQ(NormalizeLimitDomain("robot/a"), "robot/a");
  EXPECT_EQ(NormalizeLimitDomain("/robot/a/"), "robot/a");
  EXPECT_EQ(LimitDomainKeySuffix(""), "");
  EXPECT_EQ(LimitDomainKeySuffix("robot"), "/robot");
}

TEST(ZenohSerializedCaptureTest, RejectsTruncationAndExcludesSharedMemoryPadding) {
  using ibmw::extensions::zenoh_extension::DecodeZenohPayload;
  using ibmw::common::util::BufferOutOfBoundsException;
  // The length prefix is parsed by the shared GetLengthPrefixedPayload.
  EXPECT_THROW(DecodeZenohPayload("abc"), BufferOutOfBoundsException);
  auto bytes = Envelope("json", "{}");
  EXPECT_THROW(DecodeZenohPayload(std::string_view(bytes).substr(0, bytes.size() - 1)), BufferOutOfBoundsException);
  bytes.append(100, 'x');
  const auto decoded = DecodeZenohPayload(bytes);
  EXPECT_EQ(decoded.serialization, "json");
  EXPECT_EQ(decoded.payload, "{}");
  EXPECT_ANY_THROW(DecodeZenohPayload(Envelope("", "{}")));
}

TEST_F(ZenohDiscoveryTest, NativeCaptureAttachesAfterStartAndDrainsCallbackBeforeDetach) {
  Business sender(server_, "robot");
  ASSERT_TRUE(sender.Add("/capture", EndpointRole::kPublisher));
  // The bus takes registrations only before its drivers start.
  ASSERT_TRUE(sender.bus->RegisterPublisher("custom/capture/address", false));
  sender.driver.Start();
  Business receiver(client_, "robot");
  receiver.driver.Start();
  transport::SerializedSubscriberOptionsV2 options{
      .topic = {.topic = "/capture", .msg_type = "test:Opaque", .serialization_type = "cdr"},
      .wire_topic_name = "transport/%2Fcapture/test%3AOpaque/robot", .wire_type_name = "test:Opaque"};
  // The discovered address is authoritative; capture must not reconstruct it
  // from the logical topic/type or append a second domain suffix.
  options.wire_topic_name = "custom/capture/address";
  std::promise<void> entered, release;
  auto ready = entered.get_future();
  auto released = release.get_future().share();
  std::atomic<unsigned> count = 0;
  std::shared_ptr<ibmw::util::BufferArrayView> retained;
  const auto handle = receiver.driver.AttachSerializedSubscriber(options, [&](auto&& message) {
    if (++count == 1) {
      retained = message.payload;
      EXPECT_EQ(message.topic.serialization_type, "json");
      EXPECT_FALSE(message.identity.IsValid());
      entered.set_value();
      released.wait();
    }
  });
  ASSERT_TRUE(handle);
  auto bytes = Envelope("json", R"({"sequence":1})");
  const auto deadline = Clock::now() + 4s;
  while (ready.wait_for(30ms) != std::future_status::ready && Clock::now() < deadline) {
    sender.bus->Send(options.wire_topic_name, bytes.data(), bytes.size());
  }
  if (ready.wait_for(0ms) != std::future_status::ready) {
    release.set_value();
    FAIL() << "Native subscriber received no samples";
  }
  auto detached = std::async(std::launch::async, [&] { return receiver.driver.DetachSerializedSubscriber(handle); });
  EXPECT_EQ(detached.wait_for(50ms), std::future_status::timeout);
  release.set_value();
  EXPECT_TRUE(detached.get());
  const auto stopped_count = count.load();
  sender.bus->Send(options.wire_topic_name, bytes.data(), bytes.size());
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(count.load(), stopped_count);
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->JoinToString(), R"({"sequence":1})");
}

TEST_F(ZenohDiscoveryTest, NativeCaptureRejectsBroadKeysAndDoesNotMixTopics) {
  Business sender(server_, "robot");
  // The bus takes registrations only before its drivers start.
  for (const char* key : {"transport/exact", "transport/other", "transport/exact/child"}) {
    ASSERT_TRUE(sender.bus->RegisterPublisher(key, false));
  }
  sender.driver.Start();
  Business receiver(client_, "robot");
  receiver.driver.Start();
  transport::SerializedSubscriberOptionsV2 options{
      .topic = {.topic = "/capture", .msg_type = "test:Opaque", .serialization_type = "json"},
      .wire_topic_name = "transport/exact", .wire_type_name = "test:Opaque"};
  for (const auto& key : std::vector<std::string>{"transport/**", "transport/*",
           "transport/$*", std::string("transport/exact\0/other", 22), "transport//exact"}) {
    options.wire_topic_name = key;
    const auto invalid = receiver.driver.AttachSerializedSubscriber(options, [](auto&&) {});
    EXPECT_FALSE(invalid);
    if (invalid) receiver.driver.DetachSerializedSubscriber(invalid);
  }
  options.wire_topic_name = "transport/exact";
  std::atomic<unsigned> received = 0, polluted = 0;
  const auto handle = receiver.driver.AttachSerializedSubscriber(options, [&](auto&& message) {
    if (message.payload->JoinToString() != "selected") ++polluted;
    ++received;
  });
  ASSERT_TRUE(handle);
  auto selected = Envelope("json", "selected");
  auto unrelated = Envelope("json", "unrelated");
  const auto deadline = Clock::now() + 1s;
  do {
    sender.bus->Send("transport/other", unrelated.data(), unrelated.size());
    sender.bus->Send("transport/exact/child", unrelated.data(), unrelated.size());
    sender.bus->Send(options.wire_topic_name, selected.data(), selected.size());
    std::this_thread::sleep_for(20ms);
  } while (Clock::now() < deadline);
  ASSERT_TRUE(receiver.driver.DetachSerializedSubscriber(handle));
  EXPECT_GT(received.load(), 0u);
  EXPECT_EQ(polluted.load(), 0u);
}

TEST_F(ZenohDiscoveryTest, FailedNativeRegistrationIsNotAdvertised) {
  auto bus = std::make_shared<ZenohBus>();
  bus->Initialize(server_, 1024 * 1024);
  // A leading slash is invalid in a Zenoh key expression. Do not advertise a
  // failed endpoint or silently continue with an invalid owned handle.
  EXPECT_FALSE(bus->RegisterPublisher("/invalid", false));
  EXPECT_FALSE(bus->RegisterSubscriber("/invalid", [](const auto*) {}));
  bus->StartDiscovery("");
  auto provider = MakeProvider();
  provider->Start({.backend = DiscoveryBackend::kZenoh, .zenoh_config = client_});
  EXPECT_TRUE(provider->Query(Clock::now() + 500ms).endpoints.empty());
  bus->Shutdown();
  bus->Shutdown();
}

TEST_F(ZenohDiscoveryTest, ReplacesInstanceSnapshotsAndPreservesGoodRepliesOnError) {
  zenoh_detail::Owned<z_owned_session_t> session;
  zenoh_detail::OpenSession(session.value, server_);
  ZenohDiscoveryResponder responder;
  responder.Record(EndpointRole::kPublisher, "/old", "T", "old");
  responder.Start(z_loan(session.value), "robot");
  auto provider = MakeProvider();
  provider->Start({.backend = DiscoveryBackend::kZenoh, .zenoh_domain = "robot", .zenoh_config = client_});
  const auto original = WaitFor(*provider, 1);
  ASSERT_EQ(original.endpoints.size(), 1u);

  responder.Clear();
  EXPECT_TRUE(WaitFor(*provider, 0).endpoints.empty());
  responder.Record(EndpointRole::kSubscriber, "/new", "T", "new");
  const auto replaced = WaitFor(*provider, 1);
  ASSERT_EQ(replaced.endpoints.size(), 1u);
  EXPECT_EQ(replaced.endpoints.front().topic_name, "/new");
  EXPECT_NE(replaced.endpoints.front().endpoint_id, original.endpoints.front().endpoint_id);

  // A second queryable returns malformed metadata while the good responder
  // remains active. The source must report failure without losing good data.
  const auto bad_key = zenoh_protocol::SnapshotKey("robot", "bad");
  z_view_keyexpr_t key;
  ASSERT_EQ(z_view_keyexpr_from_str(&key, bad_key.c_str()), Z_OK);
  zenoh_detail::Owned<z_owned_queryable_t> queryable;
  zenoh_detail::Owned<z_owned_closure_query_t> callback;
  zenoh_detail::Owned<z_owned_fifo_handler_query_t> queries;
  z_fifo_channel_query_new(&callback.value, &queries.value, 16);
  ASSERT_EQ(z_declare_queryable(z_loan(session.value), &queryable.value,
      z_loan(key), z_move(callback.value), nullptr), Z_OK);
  auto result = replaced;
  const auto deadline = Clock::now() + 3s;
  while (Clock::now() < deadline && result.Succeeded()) {
    zenoh_detail::Owned<z_owned_query_t> query;
    if (z_fifo_handler_query_try_recv(z_loan(queries.value), &query.value) == Z_OK) {
      zenoh_detail::Owned<z_owned_bytes_t> bytes;
      ASSERT_EQ(z_bytes_copy_from_str(&bytes.value, "{}"), Z_OK);
      ASSERT_EQ(z_query_reply(z_loan(query.value), z_loan(key), z_move(bytes.value), nullptr), Z_OK);
    }
    result = provider->Query(Clock::now() + 20ms);
  }
  EXPECT_FALSE(result.Succeeded());
  ASSERT_EQ(result.endpoints.size(), 1u);
  EXPECT_EQ(result.endpoints.front().topic_name, "/new");
  EXPECT_EQ(result.sources.front().status, DiscoveryStatus::kError);
}
}  // namespace
}  // namespace ibmw::discovery
