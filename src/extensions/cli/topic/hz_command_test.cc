// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "rate_statistics.h"
#include "topic_command.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>
#include <unistd.h>

#include "engine/runtime_graph/runtime_graph_agent.h"
#include "engine/topic_io/runtime_tap_stream.h"
#include "engine/topic_io/topic_subscription.h"
#include "gtest/gtest.h"

namespace ibmw::cli::topic {
namespace {
using namespace std::chrono_literals;
namespace graph = ibmw::runtime::core::runtime_graph;
namespace io = ibmw::runtime::core::topic_io;

TEST(RateStatisticsTest, ComputesPopulationDeviationAndEvictsOldIntervals) {
  RateStatistics statistics(3);
  const auto start = RateStatistics::Clock::time_point{};
  statistics.Observe(start);
  EXPECT_EQ(statistics.Snapshot().window, 1u);
  statistics.Observe(start + 100ms);
  statistics.Observe(start + 300ms);
  auto result = statistics.Snapshot();
  EXPECT_NEAR(result.rate, 1.0 / 0.15, 1e-10);
  EXPECT_NEAR(result.minimum, 0.1, 1e-10);
  EXPECT_NEAR(result.maximum, 0.2, 1e-10);
  EXPECT_NEAR(result.deviation, 0.05, 1e-10);
  statistics.Observe(start + 700ms);
  result = statistics.Snapshot();
  EXPECT_EQ(result.received, 4u);
  EXPECT_EQ(result.window, 3u);
  EXPECT_NEAR(result.rate, 1.0 / 0.3, 1e-10);
  EXPECT_NEAR(result.minimum, 0.2, 1e-10);
  EXPECT_NEAR(result.maximum, 0.4, 1e-10);
  EXPECT_NEAR(result.deviation, 0.1, 1e-10);
}

TEST(RateStatisticsTest, HandlesEmptyEqualAndRegressingTimestamps) {
  EXPECT_THROW(RateStatistics(1), std::invalid_argument);
  RateStatistics statistics(2);
  EXPECT_EQ(statistics.Snapshot().window, 0u);
  const auto start = RateStatistics::Clock::time_point{};
  statistics.Observe(start + 100ms);
  statistics.Observe(start + 100ms);
  EXPECT_TRUE(std::isfinite(statistics.Snapshot().rate));
  statistics.Observe(start);
  EXPECT_EQ(statistics.Snapshot().window, 1u);
  statistics.Observe(start + 10ms);
  EXPECT_NEAR(statistics.Snapshot().rate, 100.0, 1e-10);
}

struct CommandResult { int code; std::string output; std::string error; };
CommandResult Invoke(std::vector<std::string> arguments) {
  arguments.insert(arguments.begin(), "topic");
  std::vector<const char*> argv;
  for (const auto& argument : arguments) argv.push_back(argument.c_str());
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const int code = RunTopicCommand(static_cast<int>(argv.size()), argv.data());
  return {code, testing::internal::GetCapturedStdout(), testing::internal::GetCapturedStderr()};
}

TEST(HzCommandTest, ExposesHelpAndRejectsInvalidOptionsBeforeStarting) {
  const auto help = Invoke({"hz", "--help"});
  EXPECT_EQ(help.code, 0);
  for (const std::string flag : {"--window", "--duration", "--type", "--registry",
                                 "--dds", "--ros2", "--zenoh", "--config-file"}) {
    EXPECT_NE(help.output.find(flag), std::string::npos);
  }
  const std::vector<std::vector<std::string>> invalid{
      {}, {"--window", "1"}, {"--window=0"}, {"--window=-1"}, {"--window=3.5"},
      {"--window", "2", "--window", "3"}, {"--duration=nan"}, {"--duration=inf"},
      {"--duration=0"}, {"--timeout=-1"}, {"--timeout=1x"}, {"--type="},
      {"--dds", "--ros2"}, {"--all"}, {"--qos-reliability", "reliable"},
      {"--dds", "--qos-durability", "invalid"},
      {"--zenoh", "--qos-reliability", "reliable"}, {"--config-file", "missing"}};
  for (auto arguments : invalid) {
    SCOPED_TRACE(testing::PrintToString(arguments));
    if (!arguments.empty()) arguments.insert(arguments.begin(), "/test");
    arguments.insert(arguments.begin(), "hz");
    const auto result = Invoke(std::move(arguments));
    EXPECT_EQ(result.code, 1);
    EXPECT_TRUE(result.output.empty());
    EXPECT_FALSE(result.error.empty());
  }
}

class TestTap final : public io::PublishTapCapability {
 public:
  uint64_t Register(Callback callback) override {
    std::lock_guard lock(mutex_);
    callback_ = std::move(callback);
    return 1;
  }
  bool Unregister(uint64_t) override {
    std::lock_guard lock(mutex_);
    callback_ = {};
    return true;
  }
  void Emit() {
    std::lock_guard lock(mutex_);
    if (!callback_) return;
    const std::string bytes = "payload";
    ibmw::transport::Context context;
    ibmw::extension::MessageView view{
        .topic = "/rate", .msg_type = "test:Bytes", .module_name = "publisher", .endpoint_index = 0,
        .context = ibmw::transport::ContextRef(context),
        .available_serialization_types = {"cdr"},
        .acquire_serialized = [&](std::string_view) {
          return std::make_shared<ibmw::util::BufferArrayView>(bytes.data(), bytes.size());
        }};
    callback_(view);
  }
 private:
  std::mutex mutex_;
  Callback callback_;
};

class LocalHzTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = "/tmp/ibmw-hz-test-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    directory = pattern;
    server = std::make_unique<io::RuntimeTapServer>(tap, "hz-test", directory);
    server->Start();
    graph::RuntimeRegistration registration{.runtime_id = "hz-test"};
    registration.topics.push_back({.publisher_id = "publisher-0",
        .topic = {.topic = "/rate", .msg_type = "test:Bytes", .serialization_type = "cdr"},
        .capture_capabilities = {{"runtime-tap", {.mode = "runtime_tap"}}}});
    RestartAgent(registration);
  }
  void RestartAgent(const graph::RuntimeRegistration& registration) {
    agent.reset();
    agent = std::make_unique<graph::RuntimeGraphAgent>(graph::RuntimeGraphAgent::Options{
        .registration = registration, .registry_path = directory});
    agent->SetStartTapHandler([&](const auto& advertisement) { return server->StartSession(advertisement); });
    agent->SetStopTapHandler([&](auto id) { return server->StopSession(id); });
    agent->Initialize();
    agent->Start();
  }
  void TearDown() override {
    agent.reset();
    server.reset();
    std::filesystem::remove_all(directory);
  }
  std::filesystem::path directory;
  TestTap tap;
  std::unique_ptr<io::RuntimeTapServer> server;
  std::unique_ptr<graph::RuntimeGraphAgent> agent;
};

TEST_F(LocalHzTest, MeasuresRealTapStreamAndDetachesOnDuration) {
  std::jthread publisher([&](std::stop_token stop) {
    while (!stop.stop_requested()) { tap.Emit(); std::this_thread::sleep_for(20ms); }
  });
  const auto result = Invoke({"hz", "/rate", "--registry-path", directory.string(),
                              "--duration", "0.3", "--window", "10"});
  EXPECT_EQ(result.code, 0) << result.error;
  const auto position = result.output.find("average rate: ");
  ASSERT_NE(position, std::string::npos) << result.output;
  const auto rate = std::stod(result.output.substr(position + 14));
  EXPECT_GT(rate, 25);
  EXPECT_LT(rate, 75);
  EXPECT_NE(result.output.find("window: 10"), std::string::npos);
  EXPECT_NE(result.error.find("Subscribed to /rate"), std::string::npos);
  // A second invocation must be able to open a new session after cleanup.
  EXPECT_EQ(Invoke({"hz", "/rate", "--registry-path", directory.string(),
                    "--duration", "0.1"}).code, 0);
}

TEST_F(LocalHzTest, ReportsNoMessagesAndMissingPublisher) {
  const auto silent = Invoke({"hz", "/rate", "--registry-path", directory.string(),
                              "--duration", "1.05"});
  EXPECT_EQ(silent.code, 0) << silent.error;
  EXPECT_NE(silent.output.find("no new messages"), std::string::npos);
  const auto missing = Invoke({"hz", "/missing", "--registry-path", directory.string(), "--timeout", "0.05"});
  EXPECT_EQ(missing.code, 1);
  EXPECT_NE(missing.error.find("Timed out waiting for a publisher"), std::string::npos);
}

TEST_F(LocalHzTest, RejectsAmbiguousPublishedTypesAndAcceptsExplicitType) {
  auto registration = agent->Registration();
  registration.topics.push_back({.publisher_id = "other-0",
      .topic = {.topic = "/rate", .msg_type = "test:Other", .serialization_type = "cdr"}});
  RestartAgent(registration);
  const auto ambiguous = Invoke({"hz", "/rate", "--registry-path", directory.string(), "--duration", "0.05"});
  EXPECT_EQ(ambiguous.code, 1);
  EXPECT_NE(ambiguous.error.find("multiple types"), std::string::npos);
  const auto selected = Invoke({"hz", "/rate", "--registry-path", directory.string(),
                                "--type", "test:Bytes", "--duration", "0.05"});
  EXPECT_EQ(selected.code, 0) << selected.error;
  // Subscriber-only types do not make a published topic ambiguous.
  registration.topics.back().published = false;
  RestartAgent(registration);
  EXPECT_EQ(Invoke({"hz", "/rate", "--registry-path", directory.string(), "--duration", "0.05"}).code, 0);
}

TEST_F(LocalHzTest, FollowsPublisherRemovalAndReappearanceWithoutDuplicateTaps) {
  io::TopicSubscription subscription({.source = {.registry_path = directory}, .topic = "/rate"});
  std::atomic<unsigned> received{0};
  subscription.Start([&](auto&&) { ++received; });
  ASSERT_TRUE(subscription.Refresh());
  ASSERT_TRUE(subscription.Refresh());
  const auto await_count = [&](unsigned count) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (received.load() < count && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(received.load(), count);
  };
  tap.Emit();
  await_count(1);
  auto registration = agent->Registration();
  auto empty = registration;
  empty.topics.clear();
  RestartAgent(empty);
  EXPECT_FALSE(subscription.Refresh());
  tap.Emit();
  EXPECT_EQ(received.load(), 1u);
  RestartAgent(registration);
  ASSERT_TRUE(subscription.Refresh());
  tap.Emit();
  await_count(2);
  subscription.Stop();
  tap.Emit();
  EXPECT_EQ(received.load(), 2u);
}

TEST_F(LocalHzTest, ConnectionConfigLoadsOnlySelectedTransportAndResolvesPaths) {
  const auto path = directory / "connection.yaml";
  {
    std::ofstream file(path);
    file << R"(ibmw:
  extensions:
    items:
      - name: dds_extension
        path: libdds.so
        options:
          domain_id: 99
          xml_profile_file: profiles.xml
      - name: unrelated_extension
        path: unrelated.so
  messaging:
    drivers:
      - type: local
      - type: dds
  modules:
    pkgs:
      - path: business.so
)";
  }
  ibmw::discovery::DiscoveryOptions source{.backend = ibmw::discovery::DiscoveryBackend::kDds, .domain_id = 17};
  const auto root = io::SubscriptionRuntimeConfig(source, path);
  const auto runtime = root["ibmw"];
  EXPECT_FALSE(runtime["modules"]);
  EXPECT_FALSE(runtime["runtime_graph"]["enabled"].as<bool>());
  ASSERT_EQ(runtime["extensions"]["items"].size(), 1u);
  const auto extension = runtime["extensions"]["items"][0];
  EXPECT_EQ(extension["path"].as<std::string>(), (directory / "libdds.so").string());
  EXPECT_EQ(extension["options"]["xml_profile_file"].as<std::string>(), (directory / "profiles.xml").string());
  EXPECT_EQ(extension["options"]["domain_id"].as<unsigned>(), 17u);
  ASSERT_EQ(runtime["messaging"]["drivers"].size(), 1u);
  EXPECT_EQ(runtime["messaging"]["drivers"][0]["type"].as<std::string>(), "dds");
}

}  // namespace
}  // namespace ibmw::cli::topic
