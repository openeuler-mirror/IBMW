// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "echo_formatter.h"
#include "observation_test_support.h"

#include <fstream>
#include <thread>

#include "nlohmann/json.hpp"
#include "yaml-cpp/yaml.h"

namespace ibmw::cli::topic {
namespace {
using namespace std::chrono_literals;
using test::Invoke;
using Message = ibmw::transport::SerializedMessage;

Message Payload(std::string bytes, std::string format = "json") {
  auto storage = std::make_shared<std::string>(std::move(bytes));
  auto* view = new ibmw::util::BufferArrayView(storage->data(), storage->size());
  return {.topic = {.topic = "/echo", .msg_type = "test:Bytes", .serialization_type = std::move(format)},
          .payload = std::shared_ptr<ibmw::util::BufferArrayView>(view,
              [storage](auto* owned) { delete owned; })};
}

TEST(EchoFormatterTest, PreservesStructuredValuesAndSelectsNestedArrayFields) {
  const auto message = Payload(R"({"text":"true","number":18446744073709551615,"items":[{"value":42}],"empty":null,"active":true})");
  EchoFormatter yaml("yaml", "", false);
  const auto document = YAML::Load(yaml.Format(message));
  EXPECT_EQ(document["text"].as<std::string>(), "true");
  EXPECT_EQ(document["text"].Tag(), "!");
  EXPECT_EQ(document["number"].as<uint64_t>(), UINT64_MAX);
  EXPECT_TRUE(document["empty"].IsNull());
  EXPECT_TRUE(document["active"].as<bool>());
  EchoFormatter field("json", "items.0.value", false);
  EXPECT_EQ(field.Format(message), "42");
  EchoFormatter missing("yaml", "absent", false);
  EXPECT_THROW(missing.Format(message), std::runtime_error);
  EchoFormatter bad_index("yaml", "items.2", false);
  EXPECT_THROW(bad_index.Format(message), std::runtime_error);
  EXPECT_THROW(EchoFormatter("yaml", "items..value", false), std::invalid_argument);
}

TEST(EchoFormatterTest, HandlesYamlAndRejectsMalformedDocuments) {
  EchoFormatter formatter("json", "", false);
  const auto value = nlohmann::json::parse(formatter.Format(Payload(
      "number: 3\ntext: '3'\nactive: true\nitems: [null, 2]\n", "yaml")));
  EXPECT_EQ(value["number"], 3);
  EXPECT_EQ(value["text"], "3");
  EXPECT_EQ(value["active"], true);
  EXPECT_TRUE(value["items"][0].is_null());
  EXPECT_THROW(formatter.Format(Payload("[", "json")), nlohmann::json::parse_error);
  EXPECT_THROW(formatter.Format(Payload("a: [", "yaml")), YAML::Exception);
  EXPECT_THROW(formatter.Format(Payload("a: 1\n---\na: 2", "yaml")), std::runtime_error);
  EXPECT_THROW(formatter.Format(Payload("a: 1\na: 2", "yaml")), std::runtime_error);
  EXPECT_THROW(formatter.Format(Payload("&a {value: *a}", "yaml")), std::runtime_error);
}

TEST(EchoFormatterTest, PrintsRawSegmentedPayloadWithoutDecoding) {
  EchoFormatter formatter("yaml", "", true);
  const unsigned char first[] = {0, 255};
  const unsigned char second[] = {16, 127};
  const std::vector<ibmw_buffer_view_t> buffers{{first, sizeof(first)}, {nullptr, 0}, {second, sizeof(second)}};
  auto message = Payload("", "unknown");
  message.payload = std::make_shared<ibmw::util::BufferArrayView>(buffers);
  EXPECT_EQ(formatter.Format(message), "00 ff 10 7f");
  EXPECT_EQ(formatter.Format(Payload("", "unknown")), "");
  EXPECT_THROW(formatter.Format(Message{}), std::runtime_error);
}

TEST(EchoFormatterTest, LoadsBinaryDecoderAndRejectsUnsupportedFormats) {
  EchoFormatter formatter("json", "", false);
  EXPECT_THROW(formatter.Format(Payload("data", "test_binary")), std::runtime_error);
  formatter.LoadPackage(ECHO_TYPE_SUPPORT_FIXTURE_PATH);
  EXPECT_EQ(formatter.Format(Payload("\x01hello", "test_binary")), R"({"value":"hello"})");
  EXPECT_THROW(formatter.Format(Payload("malformed", "test_binary")), std::runtime_error);
  EXPECT_THROW(formatter.Format(Payload("data", "cdr")), std::runtime_error);
}

TEST(EchoFormatterTest, DecodesRosMessagesDiscoveredThroughDdsWithoutChangingIdentity) {
  EchoFormatter formatter("json", "", false);
  formatter.LoadPackage(ECHO_TYPE_SUPPORT_FIXTURE_PATH);
  for (const auto* type : {"dds:fixture_msgs::msg::dds_::Text_",
                           "dds:fixture_msgs::srv::dds_::Query_Request_",
                           "dds:fixture_msgs::action::dds_::Move_FeedbackMessage_"}) {
    SCOPED_TRACE(type);
    auto message = Payload("\x01hello", "cdr");
    message.topic.msg_type = type;
    message.source_driver = "dds";
    EXPECT_EQ(formatter.Format(message), R"({"value":"hello"})");
    EXPECT_EQ(message.topic.msg_type, type);
    EXPECT_EQ(message.topic.serialization_type, "cdr");
  }
  EchoFormatter field("yaml", "value", false);
  field.LoadPackage(ECHO_TYPE_SUPPORT_FIXTURE_PATH);
  auto message = Payload("\x01hello", "cdr");
  message.topic.msg_type = "dds:fixture_msgs::msg::dds_::Text_";
  message.source_driver = "dds";
  EXPECT_EQ(YAML::Load(field.Format(message)).as<std::string>(), "hello");
}

TEST(EchoFormatterTest, KeepsExactDdsDecoderAuthoritative) {
  EchoFormatter formatter("json", "", false);
  // A previously loaded ROS decoder must not hide an exact native decoder.
  formatter.LoadPackage(ECHO_TYPE_SUPPORT_FIXTURE_PATH);
  formatter.LoadPackage(ECHO_NATIVE_DECODER_FIXTURE_PATH);
  auto message = Payload("\x02native", "cdr");
  message.topic.msg_type = "dds:fixture_msgs::msg::dds_::Text_";
  message.source_driver = "dds";
  EXPECT_EQ(formatter.Format(message), R"({"value":"native"})");
  // Do not retry with the ROS decoder after the exact decoder rejects bytes.
  auto rejected = Payload("\x01ros", "cdr");
  rejected.topic.msg_type = message.topic.msg_type;
  rejected.source_driver = "dds";
  EXPECT_THROW(formatter.Format(rejected), std::runtime_error);
  // Nor after an exact decoder rejects the requested format.
  rejected.topic.msg_type = "dds:fixture_msgs::msg::dds_::Unsupported_";
  EXPECT_THROW(formatter.Format(rejected), std::runtime_error);
}

TEST(EchoFormatterTest, RequiresLoadedRosDecoderAndDdsCdrBeforeAdapting) {
  auto message = Payload("\x01hello", "cdr");
  message.topic.msg_type = "dds:fixture_msgs::msg::dds_::Text_";
  message.source_driver = "dds";
  EchoFormatter formatter("json", "", false);
  EXPECT_THROW(formatter.Format(message), std::runtime_error);
  formatter.LoadPackage(ECHO_TYPE_SUPPORT_FIXTURE_PATH);
  message.source_driver = "zenoh";
  EXPECT_THROW(formatter.Format(message), std::runtime_error);
  message.source_driver = "dds";
  message.topic.serialization_type = "test_binary";
  EXPECT_THROW(formatter.Format(message), std::runtime_error);
  message.topic.serialization_type = "cdr";
  for (const auto* type : {"dds:fixture_msgs::msg::dds_::Missing_", "dds:fixture_msgs::Text",
                           "dds:fixture_msgs::msg::dds_::WrongFormat_",
                           "dds:fixture_msgs::msg::dds_::Text", "dds:fixture_msgs::msg::dds_::_",
                           "dds:fixture_msgs::other::dds_::Text_", "dds:fixture_msgs/msg/dds_/Text_"}) {
    SCOPED_TRACE(type);
    message.topic.msg_type = type;
    EXPECT_THROW(formatter.Format(message), std::runtime_error);
  }
  auto malformed = Payload("invalid CDR", "cdr");
  malformed.topic.msg_type = "dds:fixture_msgs::msg::dds_::Text_";
  malformed.source_driver = "dds";
  EXPECT_THROW(formatter.Format(malformed), std::runtime_error);
}

TEST(EchoCommandTest, ExposesEchoOptionsWithoutStatisticsAndRejectsInvalidArguments) {
  const auto help = Invoke({"echo", "--help"});
  ASSERT_EQ(help.code, 0);
  for (const std::string flag : {"--once", "--field", "--format", "--raw", "--type-support",
                                 "--registry", "--dds", "--ros2", "--zenoh", "--duration"}) {
    EXPECT_NE(help.output.find(flag), std::string::npos);
  }
  EXPECT_EQ(help.output.find("--window"), std::string::npos);
  const std::vector<std::vector<std::string>> invalid{
      {}, {"--window", "10"}, {"--field="}, {"--field", "header..stamp"},
      {"--format", "cdr"}, {"--format", "json", "--format", "yaml"},
      {"--raw", "--field", "value"}, {"--raw", "--format", "json"},
      {"--type-support="}, {"--type-support", "/missing/decoder.so"},
      {"--dds", "--registry"}, {"--all"}, {"--timeout=nan"}};
  for (auto arguments : invalid) {
    SCOPED_TRACE(testing::PrintToString(arguments));
    if (!arguments.empty()) arguments.insert(arguments.begin(), "/test");
    arguments.insert(arguments.begin(), "echo");
    const auto result = Invoke(std::move(arguments));
    EXPECT_EQ(result.code, 1);
    EXPECT_TRUE(result.output.empty());
    EXPECT_FALSE(result.error.empty());
  }
}

class RegistryEchoTest : public test::RegistryObservationTest {
 protected:
  void SetFormat(const std::string& format, const std::string& package = {}) {
    auto registration = agent->Registration();
    registration.topics.front().topic.serialization_type = format;
    registration.topics.front().type_support_package = package;
    RestartAgent(registration);
  }
  test::CommandResult Receive(std::string bytes, std::string format,
                              std::vector<std::string> arguments = {}) {
    std::jthread publisher([&](std::stop_token stop) {
      while (!stop.stop_requested()) { tap.Emit(bytes, format); std::this_thread::sleep_for(5ms); }
    });
    arguments.insert(arguments.begin(), {"echo", "/rate", "--registry-path", directory.string(), "--duration", "0.3"});
    return Invoke(std::move(arguments));
  }
};

TEST_F(RegistryEchoTest, OncePrintsOneStructuredMessageAndDetaches) {
  SetFormat("json");
  const auto result = Receive(R"({"value":42,"name":"hello"})", "json", {"--once"});
  EXPECT_EQ(result.code, 0) << result.error;
  const auto documents = YAML::LoadAll(result.output);
  ASSERT_GE(documents.size(), 1u);
  EXPECT_EQ(documents.front()["value"].as<int>(), 42);
  const auto separator = result.output.find("---");
  ASSERT_NE(separator, std::string::npos);
  EXPECT_EQ(result.output.find("---", separator + 3), std::string::npos) << result.output;
  EXPECT_FALSE(tap.Emit());
  const auto selected = Receive(R"({"items":[{"value":42}]})", "json", {"--once", "--field", "items.0.value", "--format", "json"});
  EXPECT_EQ(selected.code, 0) << selected.error;
  EXPECT_EQ(selected.output, "42\n");
}

TEST_F(RegistryEchoTest, AutomaticallyLoadsRegistryDecoderAndAcceptsExplicitPackages) {
  SetFormat("test_binary", ECHO_TYPE_SUPPORT_FIXTURE_PATH);
  const auto automatic = Receive("\x01hello", "test_binary", {"--once", "--format", "json"});
  EXPECT_EQ(automatic.code, 0) << automatic.error;
  EXPECT_EQ(automatic.output, "{\"value\":\"hello\"}\n");
  SetFormat("test_binary");
  const auto explicit_package = Receive("\x01hello", "test_binary",
      {"--once", "--format", "json", "--type-support", ECHO_TYPE_SUPPORT_FIXTURE_PATH});
  EXPECT_EQ(explicit_package.code, 0) << explicit_package.error;
  EXPECT_EQ(explicit_package.output, automatic.output);
}

TEST_F(RegistryEchoTest, DecoderErrorsReachCallerAndRawWorksWithoutTypeSupport) {
  const auto missing = Receive("bytes", "cdr", {"--once"});
  EXPECT_EQ(missing.code, 1);
  EXPECT_NE(missing.error.find("No TypeSupport"), std::string::npos);
  EXPECT_TRUE(missing.output.empty());
  EXPECT_FALSE(tap.Emit());
  const auto raw = Receive(std::string("\x00\xff", 2), "cdr", {"--once", "--raw"});
  EXPECT_EQ(raw.code, 0) << raw.error;
  EXPECT_EQ(raw.output, "00 ff\n---\n");
  SetFormat("json");
  const auto malformed = Receive("[", "json", {"--once"});
  EXPECT_EQ(malformed.code, 1);
  EXPECT_TRUE(malformed.output.empty());
  EXPECT_FALSE(tap.Emit());
}

TEST_F(RegistryEchoTest, SilentPublisherAndMissingPublisherHaveDistinctResults) {
  const auto silent = Invoke({"echo", "/rate", "--registry-path", directory.string(), "--duration", "0.05"});
  EXPECT_EQ(silent.code, 0);
  EXPECT_TRUE(silent.output.empty());
  EXPECT_NE(silent.error.find("no messages received"), std::string::npos);
  const auto missing = Invoke({"echo", "/missing", "--registry-path", directory.string(), "--timeout", "0.05"});
  EXPECT_EQ(missing.code, 1);
  EXPECT_NE(missing.error.find("Timed out"), std::string::npos);
}

TEST_F(RegistryEchoTest, LoadsConfigurationPackagesRelativeToConfiguration) {
  std::filesystem::create_symlink(ECHO_TYPE_SUPPORT_FIXTURE_PATH, directory / "decoder.so");
  const auto path = directory / "decoder.yaml";
  {
    std::ofstream file(path);
    file << "ibmw:\n  runtime_graph:\n    type_support_pkgs:\n      - path: decoder.so\n";
  }
  EchoFormatter formatter("json", "value", false);
  formatter.LoadConfiguration(path);
  EXPECT_EQ(formatter.Format(Payload("\x01hello", "test_binary")), "\"hello\"");
}

}  // namespace
}  // namespace ibmw::cli::topic
