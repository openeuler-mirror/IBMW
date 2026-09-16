// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "extension_api/ibmw_cli_provider.h"
#include "engine/runtime_graph/runtime_graph_agent.h"
#include "gtest/gtest.h"

namespace {

struct CommandResult {
  int code;
  std::string output;
  std::string error;
};

CommandResult InvokeTopic(std::vector<std::string> arguments) {
  arguments.insert(arguments.begin(), "topic");
  std::vector<const char*> argv;
  for (const auto& argument : arguments) argv.push_back(argument.c_str());
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  int code = -1;
  EXPECT_NO_THROW(code = IbmwCliEntryV1().commands[0].invoke(
                      static_cast<int>(argv.size()), argv.data()));
  auto output = testing::internal::GetCapturedStdout();
  auto error = testing::internal::GetCapturedStderr();
  return {code, std::move(output), std::move(error)};
}

class RegistryTopicCommandTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = "/tmp/ibmw-topic-cli-test-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    directory_ = pattern;
    using namespace ibmw::runtime::core::runtime_graph;
    RuntimeRegistration registration{.runtime_id = "test-runtime"};
    registration.topics = {
        {.publisher_id = "publisher", .topic = {.topic = "/shared",
          .msg_type = "example/Message", .serialization_type = "cdr",
          .available_drivers = {"local", "dds"}},
          .driver_qos = {{"local", {}}, {"dds", {.policies = {{"reliability", "reliable"}}}}}},
        {.publisher_id = "subscriber", .topic = {.topic = "/shared",
          .msg_type = "example/Message"}, .published = false},
        {.publisher_id = "subscriber-only", .topic = {.topic = "/subscribe_only",
          .msg_type = "example/Other"}, .published = false},
        {.publisher_id = "hidden", .topic = {.topic = "/_hidden",
          .msg_type = "example/Message"}, .hidden = true}};
    agent_ = std::make_unique<RuntimeGraphAgent>(RuntimeGraphAgent::Options{
        .registration = std::move(registration), .registry_path = directory_});
    agent_->Initialize();
  }
  void TearDown() override {
    agent_.reset();
    std::filesystem::remove_all(directory_);
  }
  CommandResult Query(std::vector<std::string> arguments) {
    arguments.insert(arguments.end(), {"--registry-path", directory_.string()});
    return InvokeTopic(std::move(arguments));
  }

  std::filesystem::path directory_;
  std::unique_ptr<ibmw::runtime::core::runtime_graph::RuntimeGraphAgent> agent_;
};

TEST_F(RegistryTopicCommandTest, DefaultAndExplicitRegistryPreserveTopicListing) {
  const auto defaults = Query({"list"});
  EXPECT_EQ(defaults.code, 0);
  EXPECT_TRUE(defaults.error.empty());
  EXPECT_EQ(defaults.output, "/shared\n/subscribe_only\n");
  const auto explicit_registry = Query({"list", "--registry"});
  EXPECT_EQ(explicit_registry.code, 0);
  EXPECT_EQ(explicit_registry.output, defaults.output);
  EXPECT_EQ(Query({"list", "--show-types"}).output,
            "/shared [example/Message]\n/subscribe_only [example/Other]\n");
  EXPECT_EQ(Query({"list", "-c"}).output, "2\n");
  EXPECT_EQ(Query({"list", "-c", "--include-hidden-topics"}).output, "3\n");
}

TEST_F(RegistryTopicCommandTest, TypeFindAndInfoUseUnifiedRegistryEndpoints) {
  const auto type = Query({"type", "/shared", "--registry"});
  EXPECT_EQ(type.code, 0);
  EXPECT_EQ(type.output, "example/Message\n");
  const auto find = Query({"find", "example/Message"});
  EXPECT_EQ(find.code, 0);
  EXPECT_EQ(find.output, "/shared\n");
  EXPECT_EQ(Query({"find", "example/Message", "--include-hidden-topics"}).output,
            "/_hidden\n/shared\n");
  const auto info = Query({"info", "/shared"});
  EXPECT_EQ(info.code, 0);
  EXPECT_EQ(info.output, "Type: example/Message\nPublisher count: 1\nSubscription count: 1\n");
  const auto verbose = Query({"info", "/shared", "-v"});
  EXPECT_EQ(verbose.code, 0);
  for (const std::string field : {"source: registry", "runtime: test-runtime",
                                  "endpoint: publisher", "endpoint: subscriber",
                                  "drivers: local, dds", "reliability: reliable"}) {
    EXPECT_NE(verbose.output.find(field), std::string::npos);
  }
  EXPECT_EQ(Query({"info", "/_hidden"}).code, 0);
  EXPECT_EQ(Query({"info", "/_hidden", "--include-hidden-topics"}).code, 0);
}

TEST_F(RegistryTopicCommandTest, CombinedQueriesKeepRegistryResultsAndReturnFailure) {
  for (const std::string command : {"list", "type", "find", "info"}) {
    SCOPED_TRACE(command);
    std::vector<std::string> arguments{command};
    if (command != "list") arguments.push_back(command == "find" ? "example/Message" : "/shared");
    const auto local = Query(arguments);
    arguments.insert(arguments.end(), {"--registry", "-d", "-r", "--domain", "10"});
    const auto combined = Query(arguments);
    EXPECT_EQ(local.code, 0);
    EXPECT_EQ(combined.code, 1);
    EXPECT_EQ(combined.output, local.output);
    EXPECT_NE(combined.error.find("dds (domain 10)"), std::string::npos);
    EXPECT_NE(combined.error.find("ros2 (domain 10)"), std::string::npos);
    EXPECT_EQ(combined.error.find("zenoh"), std::string::npos);
  }
  const auto all = Query({"list", "-a", "--domain", "10", "--zenoh-domain", "robot_a"});
  EXPECT_EQ(all.code, 1);
  EXPECT_EQ(all.output, "/shared\n/subscribe_only\n");
  EXPECT_NE(all.error.find("zenoh (domain robot_a)"), std::string::npos);
}

TEST(TopicCommandTest, UnavailableBackendIsNotReportedAsUnknownTopic) {
  for (const std::string flag : {"-d", "--dds", "-r", "--ros2", "-z", "--zenoh"}) {
    SCOPED_TRACE(flag);
    std::vector<std::string> arguments{"info", "/missing", flag};
    if (flag == "-r" || flag == "--ros2") {
      arguments.insert(arguments.end(), {"--ros-domain", "0"});
    }
    const auto result = InvokeTopic(arguments);
    EXPECT_EQ(result.code, 1);
    EXPECT_TRUE(result.output.empty());
    EXPECT_NE(result.error.find("not implemented"), std::string::npos);
    EXPECT_EQ(result.error.find("Unknown topic"), std::string::npos);
  }
}

TEST(TopicCommandTest, AllSubcommandsExposeDiscoveryOptions) {
  for (const std::string command : {"list", "type", "find", "info"}) {
    SCOPED_TRACE(command);
    const auto result = InvokeTopic({command, "--help"});
    EXPECT_EQ(result.code, 0);
    EXPECT_TRUE(result.error.empty());
    for (const std::string flag : {"--registry", "--dds", "--ros2", "--zenoh", "--all",
                                   "--domain", "--dds-domain", "--ros-domain",
                                   "--zenoh-domain", "--zenoh-config", "--registry-path"}) {
      EXPECT_NE(result.output.find(flag), std::string::npos);
    }
  }
}

TEST(TopicCommandTest, InvalidOptionsFailBeforeDiscovery) {
  const std::vector<std::vector<std::string>> cases{
      {"list", "-d", "--domain"}, {"list", "-d", "--domain=-1"},
      {"list", "-r", "--ros-domain", "10junk"},
      {"list", "-z", "--domain", "10"},
      {"list", "--dds-domain", "10"},
      {"list", "-d", "--dds-domain", "4294967296"}};
  for (const auto& arguments : cases) {
    SCOPED_TRACE(testing::PrintToString(arguments));
    const auto result = InvokeTopic(arguments);
    EXPECT_EQ(result.code, 1);
    EXPECT_TRUE(result.output.empty());
    EXPECT_FALSE(result.error.empty());
    EXPECT_EQ(result.error.find("not implemented"), std::string::npos);
  }
}

TEST(TopicCommandTest, ReportsDiscoveryFailureAtProviderBoundary) {
  const auto entry = IbmwCliEntryV1();
  ASSERT_EQ(entry.command_count, 1u);
  const std::string invalid_path = "/tmp/" + std::string(5000, 'x');
  for (const std::string command : {"list", "type", "find", "info"}) {
    SCOPED_TRACE(command);
    std::vector<std::string> arguments{"topic", command};
    if (command != "list") arguments.push_back("example");
    arguments.insert(arguments.end(), {"--registry-path", invalid_path});
    std::vector<const char*> argv;
    for (const auto& argument : arguments) argv.push_back(argument.c_str());

    testing::internal::CaptureStderr();
    int result = 0;
    EXPECT_NO_THROW(result = entry.commands[0].invoke(
                        static_cast<int>(argv.size()), argv.data()));
    const auto error = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result, 1);
    EXPECT_NE(error.find("ibmw topic: error:"), std::string::npos);
    EXPECT_NE(error.find(invalid_path), std::string::npos);
  }
}

}  // namespace
