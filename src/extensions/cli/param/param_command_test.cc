// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "param_command.h"
#include "gtest/gtest.h"

#include <string>
#include <vector>

namespace {
struct Result { int code; std::string output, error; };
Result Invoke(std::vector<std::string> args) {
  args.insert(args.begin(), "param");
  std::vector<const char*> argv;
  for (const auto& item : args) argv.push_back(item.c_str());
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const auto code = RunParamCommand(static_cast<int>(argv.size()), argv.data());
  return {code, testing::internal::GetCapturedStdout(), testing::internal::GetCapturedStderr()};
}

TEST(ParamCommandTest, HelpUsesModuleArgumentsAndServiceBackends) {
  const auto root = Invoke({"--help"});
  EXPECT_EQ(root.code, 0);
  for (const auto* verb : {"list", "get", "set"}) EXPECT_NE(root.output.find(verb), std::string::npos);
  const auto help = Invoke({"set", "--help"});
  EXPECT_EQ(help.code, 0);
  for (const auto* flag : {"<module>", "<parameter_name>", "<value>", "--service", "--dds", "--http", "--grpc", "--mqtt", "--zenoh", "--timeout"})
    EXPECT_NE(help.output.find(flag), std::string::npos) << flag;
  for (const auto* flag : {"--runtime", "--registry", "--type-support", "--ros2"})
    EXPECT_EQ(help.output.find(flag), std::string::npos) << flag;
}

TEST(ParamCommandTest, InvalidCallsFailBeforeLoadingOrContactingABackend) {
  const auto no_backend = Invoke({"get", "car", "speed"});
  EXPECT_NE(no_backend.error.find("exactly one backend"), std::string::npos);
  EXPECT_EQ(no_backend.error.find("--ros2"), std::string::npos);
  for (const auto& args : std::vector<std::vector<std::string>>{
      {"get", "car"}, {"set", "car", "speed"}, {"get", "car", "speed"},
      {"get", "car", "speed", "--dds", "--http", "127.0.0.1:1"},
      {"get", "car", "speed", "--runtime", "car"},
      {"get", "car", "speed", "--ros2"},
      {"get", "", "speed", "--dds"}, {"get", "car", "", "--dds"},
      {"get", "car", "speed", "--dds", "--service", "json:/invalid"},
      {"get", "car", "speed", "--dds", "--service", ""},
      {"get", "car", "speed", "--dds", "--service", "a", "--service", "b"},
      {"get", "car", "speed", "--http", "127.0.0.1:1", "--domain", "1"},
      {"list", "--dds", "--filter", "["},
      {"set", "car", "speed", "0.5", "--dds", "--timeout", "nan"}}) {
    const auto result = Invoke(args);
    EXPECT_NE(result.code, 0) << testing::PrintToString(args);
    EXPECT_TRUE(result.output.empty());
    EXPECT_EQ(result.error.find("Calling "), std::string::npos);
    EXPECT_EQ(result.error.find("Waiting for client readiness"), std::string::npos);
  }
}
}  // namespace
