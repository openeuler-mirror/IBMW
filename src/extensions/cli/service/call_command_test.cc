// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "call_command.h"
#include "call_options.h"
#include "call_type_support.h"
#include "extension_api/ibmw_cli_provider.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
namespace ibmw::cli::service {
namespace {
struct Result { int code; std::string output, error; };
Result Invoke(std::vector<std::string> arguments) {
  arguments.insert(arguments.begin(), {"service", "call"});
  std::vector<const char*> argv; for (const auto& item : arguments) argv.push_back(item.c_str());
  testing::internal::CaptureStdout(); testing::internal::CaptureStderr();
  const auto code = IbmwCliEntryV1().commands[0].invoke(argv.size(), argv.data());
  return {code, testing::internal::GetCapturedStdout(), testing::internal::GetCapturedStderr()};
}
const std::vector<std::string> base = {"json:/test/Add", "json:service_call.Request -> json:service_call.Response", "{a: 1, b: 2}"};
TEST(ServiceCallTest, HelpDescribesOneShotCallsAndSeparateDeadlines) {
  const auto help = Invoke({"--help"});
  ASSERT_EQ(help.code, 0);
  for (const char* flag : {"--dds", "--ros2", "--http", "--grpc", "--mqtt", "--zenoh", "--type-support", "--timeout", "--discovery-timeout"})
    EXPECT_NE(help.output.find(flag), std::string::npos);
}
TEST(ServiceCallTest, RejectsOptionsBeforeInitializingAnyBackend) {
  for (const auto& flags : std::vector<std::vector<std::string>>{
      {}, {"--dds", "--ros2"}, {"--local"}, {"--registry"}, {"--http", "bad"},
      {"--http", "127.0.0.1:8000", "--domain", "1"}, {"--dds", "--timeout", "nan"},
      {"--dds", "--discovery-timeout", "0"}, {"--dds", "--dds-domain", "-1"},
      {"--dds", "--timeout", "1", "--timeout", "2"}, {"--dds", "--mqtt-qos", "2"},
      {"--dds", "--serialization", "json"}, {"--ros2", "--serialization", "json"},
      {"--mqtt", "tcp://127.0.0.1:1883", "--mqtt-qos", "3"},
      {"--mqtt", "ssl://127.0.0.1:8883"}}) {
    auto args = base; args.insert(args.end(), flags.begin(), flags.end());
    auto result = Invoke(args); EXPECT_NE(result.code, 0) << testing::PrintToString(flags);
    EXPECT_TRUE(result.output.empty()); EXPECT_EQ(result.error.find("Calling "), std::string::npos);
  }
}
TEST(ServiceCallTest, PayloadAndTypesAreValidatedBeforeLoadingTransport) {
  for (const auto* text : {"{unknown: 1}", "{a: 1.5}", "{a: 18446744073709551615}", "{a: bad}", "{a: 1, a: 2}", "a: 1\n---\na: 2", "[1,2]", "{a: .nan}"}) {
    auto args = base; args[2] = text;
    args.insert(args.end(), {"--http", "127.0.0.1:1", "--type-support", SERVICE_CALL_TYPES,
                            "--transport-library", "/nonexistent/transport.so"});
    const auto result = Invoke(args); EXPECT_NE(result.code, 0) << text;
    EXPECT_NE(result.error.find("request"), std::string::npos) << result.error;
    EXPECT_TRUE(result.output.empty()); EXPECT_EQ(result.error.find("Calling "), std::string::npos);
  }
  auto args = base; args[1] = "json:missing -> json:service_call.Response";
  args.insert(args.end(), {"--http", "127.0.0.1:1", "--type-support", SERVICE_CALL_TYPES});
  EXPECT_NE(Invoke(args).error.find("No TypeSupport"), std::string::npos);
  args = base; args.insert(args.end(), {"--dds", "--type-support", SERVICE_CALL_TYPES});
  EXPECT_NE(Invoke(args).error.find("native DDS"), std::string::npos);
}
TEST(ServiceCallTest, RequestConversionPreservesStringsIntegersAndEmptyObjects) {
  const auto json = nlohmann::json::parse(ParseCallRequest("{flag: 'true', number: '123', max: 18446744073709551615, nested: [{x: 2}]}"));
  EXPECT_EQ(json["flag"], "true"); EXPECT_EQ(json["number"], "123");
  EXPECT_EQ(json["max"].get<uint64_t>(), UINT64_MAX);
  EXPECT_EQ(ParseCallRequest("{}"), "{}");
}
}  // namespace
}  // namespace ibmw::cli::service
